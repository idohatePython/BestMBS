# 这个脚本是一个Abaqus的Python脚本，用于对三维TPMS（Triply Periodic Minimal Surface）结构进行前处理。
# 它包括了材料属性定义、网格节点的识别和分类、表面提取、数据导出等功能。
# 脚本中还定义了一些辅助函数，如欧氏距离计算、向量夹角计算、最近邻搜索等，以支持网格处理和分析的需求。

# 通过应用配置的 Abaqus noGUI 网关执行。

# -*- coding: utf-8 -*-
from abaqus import *
from abaqusConstants import *
import section
import regionToolset
import displayGroupMdbToolset as dgm
import part
import material
import assembly
import step
import interaction
import load
import mesh
import optimization
import job
import sketch
import visualization
import xyPlot
import displayGroupOdbToolset as dgo
import connectorBehavior
import numpy as np
from scipy.spatial import KDTree
from numpy import pi
import odbAccess
import time
import os
import csv
import json
import sys

def _script_dir():
    for arg in sys.argv:
        text = str(arg)
        if text.lower().startswith('nogui='):
            text = text.split('=', 1)[1]
        if text.lower().endswith('.py'):
            return os.path.dirname(os.path.abspath(text))
    return os.getcwd()


SCRIPT_DIR = _script_dir()
SRC_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, '..', '..', '..', '..'))
if SRC_ROOT not in sys.path:
    sys.path.insert(0, SRC_ROOT)

from mbs.infrastructure.abaqus.runtime_metadata import fill_latest_csv_offset_from_metadata

SIM_CONFIG = {}

# 材料参数
def _legacy_material(raw, defaults, name):
    """Accept old youngs_modulus/plastic_table run configurations."""
    raw = dict(raw or {})
    if 'elastic' not in raw:
        return {
            'name': raw.get('name', name),
            'density': {'enabled': False, 'value': 0.0},
            'elastic': {
                'enabled': True,
                'youngs_modulus': raw.get('youngs_modulus', defaults['youngs_modulus']),
                'poissons_ratio': raw.get('poissons_ratio', defaults['poissons_ratio']),
            },
            'plastic': {
                'enabled': True,
                'table': raw.get('plastic_table', defaults['plastic_table']),
            },
            'hyperelastic': {'enabled': False},
        }
    return raw


def _hyperelastic_table(hyper):
    model = str(hyper.get('model', 'mooney_rivlin')).lower()
    order = int(hyper.get('order', 1))
    coeff = hyper.get('coefficients', {})
    if model == 'mooney_rivlin':
        names = ('C10', 'C01', 'D1')
    elif model == 'neo_hooke':
        names = ('C10', 'D1')
    elif model == 'yeoh':
        names = ('C10', 'C20', 'C30', 'D1', 'D2', 'D3')
    elif model == 'ogden':
        names = []
        for i in range(1, order + 1):
            names.extend(('mu%d' % i, 'alpha%d' % i))
        names.extend('D%d' % i for i in range(1, order + 1))
    else:
        raise ValueError('Unsupported hyperelastic model: %s' % model)
    return model, order, (tuple(float(coeff[name]) for name in names),)


def _apply_material(slot, raw, defaults):
    value = _legacy_material(raw, defaults, 'Material-' + slot)
    material_name = 'Material-' + slot
    mdl.Material(name=material_name)
    material_obj = mdl.materials[material_name]
    density = value.get('density', {})
    if density.get('enabled', False):
        material_obj.Density(table=((float(density.get('value')),),))
    elastic = value.get('elastic', {})
    plastic = value.get('plastic', {})
    hyper = value.get('hyperelastic', {})
    if elastic.get('enabled', False):
        material_obj.Elastic(table=((
            float(elastic.get('youngs_modulus')),
            float(elastic.get('poissons_ratio')),
        ),))
        if plastic.get('enabled', False):
            material_obj.Plastic(table=tuple(
                tuple(float(v) for v in row) for row in plastic.get('table', [])
            ))
    elif hyper.get('enabled', False):
        model, order, table = _hyperelastic_table(hyper)
        model_constants = {
            'mooney_rivlin': MOONEY_RIVLIN,
            'neo_hooke': NEO_HOOKE,
            'yeoh': YEOH,
            'ogden': OGDEN,
        }
        kwargs = dict(materialType=ISOTROPIC, testData=OFF,
                      type=model_constants[model], table=table)
        if model == 'ogden':
            kwargs['n'] = order
        material_obj.Hyperelastic(**kwargs)


def material_properties():
    defaults_a = {
        'youngs_modulus': 3949.84, 'poissons_ratio': 0.40,
        'plastic_table': ((31.36, 0.0), (29.0, 0.0005), (32.0, 0.001),
                          (39.0, 0.002), (45.5, 0.006)),
    }
    defaults_b = {
        'youngs_modulus': 1703.65, 'poissons_ratio': 0.33,
        'plastic_table': ((31.02, 0.0), (20.0, 0.0005), (22.0, 0.001),
                          (27.0, 0.002), (32.0, 0.006)),
    }
    materials = SIM_CONFIG.get('materials', {})
    _apply_material('A', materials.get('A', {}), defaults_a)
    _apply_material('B', materials.get('B', {}), defaults_b)
    return

# 求a和b点之间的欧氏距离
def euc_dist(a, b):  # 欧氏距离（1*2,1*2）
    return np.linalg.norm(a - b)


# 映射函数(找权重w，使得坐标P可以表示为P=w1*P1+w2*P2+w3*P3或P=w1*P1+w2*P2)
def mapping(mtx_coord_withp, tlr_v): # mtx_coord_withp: 映射坐标矩阵（4*2 或 3*2），tlr_v: 顶点容差
    if mtx_coord_withp.shape == (4, 2):  # 三维模型进行二维映射（四边形/三角形区域插值）
        # 检查节点P是否近似与某一端点重合
        if euc_dist(mtx_coord_withp[0], mtx_coord_withp[1]) < tlr_v:
            w = [1, 0, 0]
        elif euc_dist(mtx_coord_withp[0], mtx_coord_withp[2]) < tlr_v:
            w = [0, 1, 0]
        elif euc_dist(mtx_coord_withp[0], mtx_coord_withp[3]) < tlr_v:
            w = [0, 0, 1]
        else:
            mtx_agmt = np.vstack((mtx_coord_withp[3, :] - mtx_coord_withp[1, :],
                                  mtx_coord_withp[3, :] - mtx_coord_withp[2, :],
                                  mtx_coord_withp[3, :] - mtx_coord_withp[0, :])).T # 构建齐次坐标矩阵
            w = np.linalg.solve(mtx_agmt[:, :2], mtx_agmt[:, -1]) # 求解线性方程组，得到前两个权重
            w = np.append(w, 1 - w[0] - w[1]) # 第三个权重求1-w1-w2即可
    else:  # 二维模型进行一维映射（线段插值）
        # w = np.zeros((2, 1))
        d0 = mtx_coord_withp[2, 0] - mtx_coord_withp[1, 0] # 线段总长度（端点1→端点2）
        d1 = mtx_coord_withp[0, 0] - mtx_coord_withp[1, 0] # 点P与端点1的距离
        d2 = mtx_coord_withp[2, 0] - mtx_coord_withp[0, 0] # 点P与端点2的距离
        # 检查点P是否近似与某一端点重合
        if np.abs(d1) < tlr_v:
            w = [1, 0]
        elif np.abs(d2) < tlr_v:
            w = [0, 1]
        else:
            w = [d2 / d0, d1 / d0]
    return w


# 边界识别函数（返回1表示靠近上限，-1表示靠近下限，0表示在内部）
# 如[1, -1, 0]表示在x方向靠近上限，y方向靠近下限，z方向在内部）
def identifunc(coord_lmt, coord_sgl, tlr_s):  # coord_lmt: 坐标限制矩阵（2*dim），coord_sgl: 待识别坐标点（1*dim），tlr_s: 表面容差
    if coord_lmt.shape[1] != len(coord_sgl): # 检查输入的坐标维度是否匹配
        return 'Coordinates and limitations do not match in dimension'
    else:
        dim = len(coord_sgl)  # 模型维度
        bcon = np.zeros(dim, dtype=int)  # 标记向量
        for i in range(dim):
            if coord_sgl[i] > coord_lmt[0, i] - tlr_s:  # 在x(i=0)/y(i=1)/z(i=2)超过上限
                bcon[i] = 1
            if coord_sgl[i] < coord_lmt[1, i] + tlr_s:  # 在x(i=0)/y(i=1)/z(i=2)低于下限
                bcon[i] = -1
        return bcon


# 三点共线性判断函数，返回True或False（用于最近邻搜索中判断三个点是否共线，以避免退化情况）
def collinear_3p(mtx_coord):  # 三点共线（3*2）
    k1 = (mtx_coord[0, :] - mtx_coord[1, :]) / euc_dist(mtx_coord[0, :], mtx_coord[1, :]) # 计算点0到点1的单位向量
    k2 = (mtx_coord[0, :] - mtx_coord[2, :]) / euc_dist(mtx_coord[0, :], mtx_coord[2, :]) # 计算点0到点2的单位向量
    if euc_dist(np.abs(k1), np.abs(k2)) <= 0.00001:
        return True
    else:
        return False


# 最近邻搜索函数（找到所有tgt中最近的dim个邻居，返回索引列表（tgt*dim））
def nns(coord, tgt, rng, dim, dir):  # coord: 全局节点坐标矩阵（k*3），tgt: 目标节点索引列表，rng: 范围节点索引列表，dim: 搜索维度（2或3），dir: dir=0/1/2表示删除x/y/z方向的坐标(仅dim=3时适用)
    if len(tgt) == 0 or len(rng) == 0: # 容错判断
        return 'invalid target or range input'
    else:
        # 提取范围节点坐标矩阵
        coord_rng = np.empty(shape=(0, 3))
        for j in rng:
            coord_rng = np.vstack([coord_rng, coord[j, :]])  # 将范围节点坐标逐行添加到coord_rng矩阵中
        tree = KDTree(coord_rng) # 构建KD树以加速最近邻搜索
        # 最近邻索引矩阵
        idx = np.empty(shape=(0, dim))
        for i in tgt:
            coord_tgt = coord[i, :]  # 提取目标节点坐标
            dist, idx_rng = tree.query(coord_tgt, k=dim) # 查询最近的dim个邻居，返回距离和索引
            if dim == 3:
                coord_nn = coord_rng[idx_rng.T, :] # 最近邻坐标矩阵（dim*3）
                coord_nn = np.delete(coord_nn, dir, axis=1) # 投影到2D平面
                k_x = dim
                clnr = collinear_3p(coord_nn) # 判断最近邻是否共线
                while clnr: # 如果共线，则增加搜索的邻居数量，直到找到不共线的邻居为止
                    k_x += 1
                    dist_x, idx_rng_x = tree.query(coord_tgt, k=k_x)
                    idx_rng[2] = idx_rng_x[-1]
                    coord_nn = coord_rng[idx_rng.T, :] # 更新最近邻坐标矩阵
                    clnr = collinear_3p(coord_nn)
            idx = np.vstack([idx, idx_rng]) # 将当前目标节点的最近邻索引添加到idx矩阵中

        for m in range(len(tgt)):
            for d in range(dim):
                idx[m, d] = int(rng[int(idx[m, d])]) # 将范围内的局部索引转换为全局索引
    return idx


# 商之和
def sum_quot(a, b):
    return a/b + b/a


# 倒数和
def sum_reci(a, b):
    return 1/a + 1/b


# 判断a是否在b±t范围内，返回True或False
def IsClose(a, b, t):
    # a, b：待判断的数字（浮点数）
    # t：容差（浮点数）
    if b - t <= a <= b + t:
        return True
    else:
        return False


# 两向量间夹角（弧度）
def vector_angle(a, b):
    if type(a) != np.array:
        a = np.asarray(a)
    if type(b) != np.array:
        b = np.asarray(b)
    return np.arccos(a.dot(b)/(np.linalg.norm(a) * np.linalg.norm(b)))


# 判断两个向量是否共线
def IsCollinear(a, b, t):
    # a, b：待判断的向量（元组）
    # t：角度容差（浮点数）
    if vector_angle(a, b) <= t or vector_angle(a, b) >= pi-t:
        return True
    else:
        return False


# 判断向量是否与坐标轴平行（即是否为坐标轴向量）
def IsCSVector(a, t):
    # a：待判断的向量（元组）
    # t：角度容差（浮点数）
    v_x, v_y, v_z = (1, 0, 0), (0, 1, 0), (0, 0, 1)
    if IsCollinear(a, v_x, t) or IsCollinear(a, v_y, t) or IsCollinear(a, v_z, t):
        return True
    else:
        return False


# 从点集中提取表面函数
def GetSurfaceFromNodeSet(inputNodes):
    '''
    输入：
    inputNodes：    网格节点数组
    输出：
    返回仅由 inputNodes 中节点所构成面的 MeshFaceArray
    '''
    nodesInSet = set([(a.label, a.instanceName) for a in inputNodes]) # 节点存入集合（节点号，实例名）
    FacesOnSurf = []        # 列表：存放真正的表面单元面
    FacesTouchingSurf = {}  # 字典：存所有碰到输入节点的单元面

    for n in inputNodes: # 遍历所有输入节点
        for face in n.getElemFaces(): # 遍历与该节点相连的所有单元面
            if n.instanceName:
                # 有实例名 → 面存入字典（面号，面类型，实例名）
                FacesTouchingSurf[(face.label, face.face, n.instanceName)] = face
                continue
            # 无实例名 → 面存入字典（面号，面类型）
            FacesTouchingSurf[(face.label, face.face)] = face

    # 筛选 → 只保留完全由输入节点构成的面
    for f in FacesTouchingSurf.values():
        IsOnSurf = True
        for n in f.getNodes():
            if (n.label, n.instanceName) not in nodesInSet: # 如果面上的某个节点不在输入节点集合中，则说明该面不是由输入节点构成的表面的一部分，跳出循环并继续检查下一个面
                IsOnSurf = False # 标记该面不是表面的一部分
                break
                continue
        # 排除内部面
        if len(f.getElements()) == 2:
            IsOnSurf = False
        # 通过所有检查
        if IsOnSurf == True:
            FacesOnSurf.append(f) # 将该面f添加到面列表中
            continue
    # 打包为Abaqus的MeshFaceArray格式并返回
    MyMFA = mesh.MeshFaceArray(FacesOnSurf)
    return MyMFA


def GetExteriorFacesFromPart(inputPart):
    FacesOnSurf = []
    for face in inputPart.elementFaces:
        if len(face.getElements()) == 1:
            FacesOnSurf.append(face)
    return mesh.MeshFaceArray(FacesOnSurf)


def IsBoundaryFace(face, coord_lmt, thk_l, tlr_s):
    face_nodes = face.getNodes()
    for i_dir in range(3):
        if all(IsClose(node.coordinates[i_dir], coord_lmt[0][i_dir], tlr_s) for node in face_nodes):
            return True
        if all(IsClose(node.coordinates[i_dir], coord_lmt[1][i_dir], tlr_s) for node in face_nodes):
            return True
        if i_dir == 2 and all(IsClose(node.coordinates[i_dir], -thk_l/2, tlr_s) for node in face_nodes):
            return True
        if i_dir == 2 and all(IsClose(node.coordinates[i_dir], thk_l/2, tlr_s) for node in face_nodes):
            return True
    return False


def get_repo_item_case_insensitive(repo, item_name):
    for key in repo.keys():
        if str(key).lower() == item_name.lower():
            return repo[key]
    return None


def create_surface_from_side_sets(inputPart, surface_name):
    kwargs = {}
    for side in range(1, 5):
        set_obj = get_repo_item_case_insensitive(inputPart.sets, '%s-S%d' % (surface_name, side))
        if set_obj is not None and len(set_obj.elements) > 0:
            kwargs['face%dElements' % side] = set_obj.elements

    if not kwargs:
        return False

    inputPart.Surface(name=surface_name, **kwargs)
    return True


def read_inp_elsets(input_file):
    elsets = {}
    current_name = None
    with open(input_file, 'r') as inp_file:
        for raw_line in inp_file:
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith('*'):
                current_name = None
                upper = line.upper()
                if upper.startswith('*ELSET'):
                    for token in line.split(','):
                        token = token.strip()
                        if token.upper().startswith('ELSET='):
                            current_name = token.split('=', 1)[1].strip()
                            elsets[current_name.lower()] = []
                            break
                continue
            if current_name is not None:
                labels = [item.strip() for item in line.split(',') if item.strip()]
                elsets[current_name.lower()].extend(int(item) for item in labels)
    return elsets


def create_surface_from_inp_elsets(inputPart, surface_name, elsets):
    kwargs = {}
    for side in range(1, 5):
        key = ('%s-S%d' % (surface_name, side)).lower()
        labels = elsets.get(key)
        if labels:
            kwargs['face%dElements' % side] = inputPart.elements.sequenceFromLabels(labels=tuple(labels))

    if not kwargs:
        return False

    inputPart.Surface(name=surface_name, **kwargs)
    return True


def mesh_face_side_number(face):
    """把 Abaqus MeshFace.face 转成 face1Elements/face2Elements 需要的面号。"""
    raw_side = getattr(face, 'face', None)
    try:
        side = int(raw_side)
        if 1 <= side <= 6:
            return side
    except Exception:
        pass

    text = str(raw_side).upper()
    digits = ''.join(ch for ch in text if ch.isdigit())
    if digits:
        side = int(digits[0])
        if 1 <= side <= 6:
            return side
    return None


def create_surface_from_mesh_faces(inputPart, surface_name, faces):
    """按 MeshFace.face 分组创建 Abaqus Surface。"""
    side_labels = {}
    for face in faces:
        side = mesh_face_side_number(face)
        if side is None:
            continue
        side_labels.setdefault(side, set()).add(face.label)

    kwargs = {}
    for side, labels in side_labels.items():
        kwargs['face%dElements' % side] = inputPart.elements.sequenceFromLabels(labels=tuple(sorted(labels)))

    if not kwargs:
        return False

    inputPart.Surface(name=surface_name, **kwargs)
    return True


def create_surfaces_from_generated_tet(inputPart, thk_l, tlr_s):
    """Abaqus 内部四面体化时，从外表面直接创建 Surf-all/Surf-Bdry/Surf-Cont。"""
    coords = np.asarray([node.coordinates for node in inputPart.nodes])
    coord_lmt = np.vstack([np.amax(coords, axis=0), np.amin(coords, axis=0)])

    exterior_faces = list(GetExteriorFacesFromPart(inputPart))
    boundary_faces = [face for face in exterior_faces if IsBoundaryFace(face, coord_lmt, thk_l, tlr_s)]
    contact_faces = [face for face in exterior_faces if not IsBoundaryFace(face, coord_lmt, thk_l, tlr_s)]

    ok_all = create_surface_from_mesh_faces(inputPart, 'Surf-all', exterior_faces)
    ok_bdry = create_surface_from_mesh_faces(inputPart, 'Surf-Bdry', boundary_faces)
    ok_cont = create_surface_from_mesh_faces(inputPart, 'Surf-Cont', contact_faces)
    return ok_all and ok_bdry and ok_cont


# 删除之前的分析文件（inp、sta、msg、log、lck、odb）和结果文件（result.txt），以避免干扰新的分析
def remove_prev_files(job):
    list_suffix = ['inp', 'sta', 'msg', 'log', 'lck', 'odb']
    for suffix in list_suffix:
        prev_file = './' + job + '.' + suffix
        if os.path.exists(prev_file):
            os.remove(prev_file)
    if os.path.exists('result.txt'):
        os.remove('result.txt')


def show_in_viewport(displayed_object=None):
    try:
        session.viewports['Viewport: 1'].setValues(displayedObject=displayed_object)
    except Exception:
        pass


PROGRESS_FILE = r'C:\temp\TPMS11_PrePrc_progress.log'


def progress(message):
    text = '>>> ' + str(message)
    print(text)
    sys.stdout.flush()
    try:
        with open(PROGRESS_FILE, 'a') as progress_file:
            progress_file.write(text + '\n')
    except Exception:
        pass


def abaqus_job_failed(work_dir, job_name):
    """检查 Abaqus 输入处理/求解是否已经失败，避免一直等待不存在的 STA。"""
    patterns = [
        'Abaqus/Analysis exited with errors',
        'Analysis Input File Processor exited with an error',
        'THE PROGRAM HAS DISCOVERED',
        'FATAL ERRORS',
        '***ERROR',
    ]
    for suffix in ('log', 'dat', 'msg'):
        file_path = os.path.join(work_dir, job_name + '.' + suffix)
        if not os.path.exists(file_path):
            continue
        try:
            with open(file_path, 'r', encoding='latin-1', errors='ignore') as file:
                text = file.read()
        except Exception:
            continue
        for pattern in patterns:
            if pattern in text:
                return True, file_path, pattern
    return False, None, None


def abaqus_job_completed(work_dir, job_name):
    """检查 STA 或 LOG 是否显示分析已经正常完成。"""
    sta_path = os.path.join(work_dir, job_name + '.sta')
    log_path = os.path.join(work_dir, job_name + '.log')
    for file_path, marker in (
        (sta_path, 'THE ANALYSIS HAS COMPLETED SUCCESSFULLY'),
        (log_path, 'Abaqus JOB %s COMPLETED' % job_name),
    ):
        if not os.path.exists(file_path):
            continue
        try:
            with open(file_path, 'r', encoding='latin-1', errors='ignore') as file:
                if marker in file.read():
                    return True
        except Exception:
            pass
    return False


def report_small_volume_elements(work_dir, job_name):
    """Print elements listed by Abaqus under ErrElemVolSmallNegZero."""
    dat_path = os.path.join(work_dir, job_name + '.dat')
    if not os.path.exists(dat_path):
        return
    try:
        with open(dat_path, 'r', encoding='latin-1', errors='ignore') as file:
            lines = file.readlines()
    except Exception:
        return

    latest_elements = []
    index = 0
    while index < len(lines):
        if 'Elements with small volume' not in lines[index]:
            index += 1
            continue

        table_elements = []
        index += 1
        while index < len(lines) and 'Element' not in lines[index]:
            index += 1
        index += 1

        while index < len(lines):
            line = lines[index].strip()
            if not line or line.startswith('-'):
                index += 1
                continue
            if line.startswith('*') or line.startswith('***'):
                break
            parts = line.split()
            if parts and parts[0].upper().startswith('PART-'):
                table_elements.append(parts[0])
                index += 1
                continue
            break

        if table_elements:
            latest_elements = table_elements

    if latest_elements:
        progress(
            'Abaqus small/zero/negative volume elements: %s'
            % ', '.join(latest_elements)
        )


def write_penalty_result_to_csv():
    """未找到有效 proof stress 时写入差样本，保证优化流程继续。"""
    script_path = next((arg for arg in sys.argv if str(arg).lower().endswith('.py')), '')
    script_dir = os.path.dirname(os.path.abspath(script_path)) if script_path else os.getcwd()
    file_path = os.path.abspath(os.path.join(script_dir, '..', 'data', 'hzc_guess.csv'))
    with open(file_path, 'r') as file:
        rows = list(csv.reader(file))
        rows[-1][4] = '0.000'
        rows[-1][5] = 'nan'
    with open(file_path, 'w', newline='') as file:
        csv.writer(file).writerows(rows)
    fill_latest_csv_offset_from_metadata(file_path, 'C:\\temp\\TPMS11', progress=progress)


def runtime_args():
    """读取 Abaqus noGUI 中 -- 后面的参数。"""
    if '--' in sys.argv:
        return sys.argv[sys.argv.index('--') + 1:]
    args = []
    for arg in sys.argv[1:]:
        text = str(arg)
        if text.lower().endswith('.py'):
            continue
        args.append(text)
    return args


if __name__ == '__main__':
# ====================================================================================
    # 0. 前处理准备：参数设置 / 关闭旧数据库 / 删除旧结果 / 创建新模型
    # ====================================================================================
    run_args = runtime_args()
    config_path = next((str(arg) for arg in run_args if str(arg).lower().endswith('.json')), None)
    if config_path:
        with open(config_path, 'r') as config_file:
            SIM_CONFIG = json.load(config_file)
    work_dir = os.path.abspath(SIM_CONFIG.get('work_dir', 'C:\\temp')) + os.sep
    if not os.path.isdir(work_dir):
        os.makedirs(work_dir)
    os.chdir(work_dir)
    PROGRESS_FILE = os.path.join(work_dir, 'TPMS11_PrePrc_progress.log')
    if os.path.exists(PROGRESS_FILE):
        os.remove(PROGRESS_FILE)
    progress('TPMS11_PrePrc started.')
    model_name = SIM_CONFIG.get('model_name', 'intlck-tpms')
    job_name = SIM_CONFIG.get('job_name', 'job-' + model_name)
    sta_file = job_name + '.sta'     # 分析状态文件（包含分析进度和结果摘要）
    odb_filepath = work_dir + job_name + '.odb' # ODB文件路径
    lck_filepath = work_dir + job_name + '.lck' # 锁文件路径

    mesh_name = 'tpms'                              # 网格名称
    mesh_path = os.path.abspath(SIM_CONFIG.get('mesh_dir', 'C:\\temp\\TPMS11')) + os.sep
    trimesh_name = mesh_path + mesh_name + '-tri-'  # 三角面网格文件名前缀（后续会自动添加A/B和.inp后缀）
    tetmesh_name = mesh_path + mesh_name + '-tet-'  # 四面体网格文件名前缀（后续会自动添加A/B和.inp后缀）

    ## 脚本固定参数
    StepTime = float(SIM_CONFIG.get('step_time', 1.0))
    ucs = float(SIM_CONFIG.get('wth', 10.0))
    rep_z = int(SIM_CONFIG.get('rep_z', 3))
    thk_l = ucs * rep_z
    u_tens = float(SIM_CONFIG.get('tensile_displacement', 2.0))

    tolerances = SIM_CONFIG.get('tolerances', {})
    T_v = float(tolerances.get('vertex', 0.0001))
    T_e = float(tolerances.get('edge', 0.001))
    T_s = float(tolerances.get('surface', 0.01))
    T_a = float(tolerances.get('angle', 0.1))

    postprc = int(SIM_CONFIG.get('monitor_until_complete', 0))
    for arg in run_args:
        if str(arg) in ('0', '1'):
            postprc = int(arg)
            break
    tet_backend = str(SIM_CONFIG.get('tet_backend', 'tetgen')).lower()
    for arg in run_args:
        arg_lower = str(arg).lower()
        if arg_lower in ('tetgen', 'abaqus'):
            tet_backend = arg_lower
            break
    tet = tet_backend != 'abaqus' # True=导入 TetGen 体网格；False=Abaqus 内部四面体化
    progress('Tet backend: %s' % tet_backend)

    # >>>>>>>>>>>>>>>>>> 前处理 <<<<<<<<<<<<<<<<<<<<
    # 关闭目前打开的ODB
    if len(session.odbs.keys()) == 0:
        pass
    else:
        for key in list(session.odbs.keys()):
            session.odbs[key].close()

    progress('Closing old ODBs and removing previous job files.')
    remove_prev_files(job_name)                                     # 删除上一轮分析生成的inp/sta/msg/log/lck/odb/result.txt
    Mdb()                                                            # 新建Abaqus模型数据库
    show_in_viewport(None)                                           # 清空视口显示对象
    mdb.models.changeKey(fromName='Model-1', toName=model_name)      # 将默认模型重命名为当前模型名
    mdl = mdb.models[model_name]                                     # 获取模型对象，后面统一通过mdl操作
    progress('Model database initialized.')

    # ====================================================================================
    # 1. 导入三角网格，并准备四面体网格
    # ====================================================================================
    def import_mesh_part(input_path, target_name):
        """Import an INP without assuming its *Part name is the legacy PART-1."""
        before = set(mdl.parts.keys())
        mdl.PartFromInputFile(inputFileName=input_path)
        created = [name for name in mdl.parts.keys() if name not in before]
        if len(created) != 1:
            raise RuntimeError(
                'Expected exactly one part from %s, created=%s' % (input_path, created)
            )
        mdl.parts.changeKey(fromName=created[0], toName=target_name)
        return mdl.parts[target_name]

    # 先导入外部生成好的三角表面网格A/B
    progress('Importing tri mesh A.')
    p_tri_a = import_mesh_part(trimesh_name+'A.inp', 'Part-Tri-A')
    progress('Imported tri mesh A.')
    progress('Importing tri mesh B.')
    p_tri_b = import_mesh_part(trimesh_name+'B.inp', 'Part-Tri-B')
    progress('Imported tri mesh B.')
    progress('Tri meshes imported.')

    # tet=True：直接导入外部已生成好的四面体网格
    # tet=False：由三角表面网格在 Abaqus 中直接生成四面体网格
    if tet:
        # ------------------------------
        # 1.1 直接导入四面体网格Part-Tet-A和Part-Tet-B
        # ------------------------------
        progress('Importing tet mesh A.')
        p_a = import_mesh_part(tetmesh_name+'A.inp', 'Part-Tet-A')
        show_in_viewport(p_a)
        progress('Imported tet mesh A.')
        progress('Importing tet mesh B.')
        p_b = import_mesh_part(tetmesh_name+'B.inp', 'Part-Tet-B')
        show_in_viewport(p_b)
        progress('Imported tet mesh B.')
        progress('Tet meshes imported.')
    else:
        # ------------------------------
        # 1.2 从三角网格复制并生成四面体网格
        # ------------------------------
        abaqus_mesh_config = SIM_CONFIG.get('abaqus_mesh', {})
        use_target_size = bool(abaqus_mesh_config.get('use_target_size', True))
        target_size = float(abaqus_mesh_config.get('target_size', 0.06 * ucs))
        if use_target_size:
            progress('Abaqus orphan-mesh target element size: %.6g mm.' % target_size)
        else:
            progress('Abaqus orphan-mesh target element size: automatic.')
        # 第一个部件
        progress('Generating tet mesh A in Abaqus.')
        p_a = mdl.Part(name='Part-Tet-A', objectToCopy=p_tri_a) # 第一个部件：立方体网格A
        if use_target_size:
            p_a.setElementSize(size=target_size)
        p_a.generateMesh(elemShape=TET) # 生成四面体网格
        progress('Abaqus tet mesh A: %d nodes, %d elements.' % (len(p_a.nodes), len(p_a.elements)))
        p_a.setValues(space=THREE_D, type=DEFORMABLE_BODY)              # 设置为3D可变形体
        show_in_viewport(p_a)                                           # 显示在视口
        # 第二个部件
        progress('Generating tet mesh B in Abaqus.')
        p_b = mdl.Part(name='Part-Tet-B', objectToCopy=p_tri_b) # 第二个部件：立方体网格B
        if use_target_size:
            p_b.setElementSize(size=target_size)
        p_b.generateMesh(elemShape=TET)
        progress('Abaqus tet mesh B: %d nodes, %d elements.' % (len(p_b.nodes), len(p_b.elements)))
        p_b.setValues(space=THREE_D, type=DEFORMABLE_BODY)
        show_in_viewport(p_b)
        progress('Tet meshes generated in Abaqus.')

    # 为两个体网格部件创建全集，后续赋材料/截面时直接对整个实体使用
    p_a.Set(elements=p_a.elements, name='Set-ALL')
    p_b.Set(elements=p_b.elements, name='Set-ALL')
    progress('Element sets created.')

    # ====================================================================================
    # 2. 基于三角网格识别表面，并映射到四面体网格上创建 Abaqus Surface
    # ====================================================================================
    # 目的：
    # 1) Surf-all  : 整个外表面
    # 2) Surf-Bdry : 外边界面（坐标轴方向的端面）
    # 3) Surf-Cont : 接触面（Surf-all 中去掉 Surf-Bdry 后的部分）

    def coord_key(coord, ndigits):
        return tuple(round(float(v), ndigits) for v in coord)

    def build_node_lookup(nodes, ndigits):
        node_lookup = {}
        for node in nodes:
            node_lookup[coord_key(node.coordinates, ndigits)] = node
        return node_lookup

    prt_tet = [p_a, p_b]
    required_surfaces = ['Surf-all', 'Surf-Bdry', 'Surf-Cont']
    if tet:
        tet_elsets = [
            read_inp_elsets(tetmesh_name + 'A.inp'),
            read_inp_elsets(tetmesh_name + 'B.inp')
        ]
        for i_prt, prt in enumerate(prt_tet):
            progress('Imported set keys for part %d: %s' % (i_prt + 1, list(prt.sets.keys())[:30]))
            for surface_name in required_surfaces:
                if surface_name not in prt.surfaces.keys():
                    if create_surface_from_side_sets(prt, surface_name):
                        progress('Created %s for part %d from imported elsets.' % (surface_name, i_prt + 1))
                    elif create_surface_from_inp_elsets(prt, surface_name, tet_elsets[i_prt]):
                        progress('Created %s for part %d from inp elsets.' % (surface_name, i_prt + 1))
            if not any(name in prt.surfaces.keys() for name in required_surfaces):
                if create_surfaces_from_generated_tet(prt, thk_l, T_s):
                    progress(
                        'Created Surf-all/Surf-Bdry/Surf-Cont for imported part %d '
                        'from its exterior tetra faces.' % (i_prt + 1)
                    )
    else:
        for i_prt, prt in enumerate(prt_tet):
            if create_surfaces_from_generated_tet(prt, thk_l, T_s):
                progress('Created Surf-all/Surf-Bdry/Surf-Cont for part %d from Abaqus generated tet mesh.' % (i_prt + 1))

    for i_prt, prt in enumerate(prt_tet):
        progress('Imported surface keys for part %d: %s' % (i_prt + 1, list(prt.surfaces.keys())))
    imported_surfaces_ok = all(
        all(surface_name in prt.surfaces.keys() for surface_name in required_surfaces)
        for prt in prt_tet
    )
    if imported_surfaces_ok:
        progress('Tet mesh surfaces are ready.')
    else:
        raise RuntimeError('Tet mesh surfaces Surf-all/Surf-Bdry/Surf-Cont were not created.')

    # PartFromInputFile preserves the element type declared by the generated
    # INP (C3D4 or C3D10).  Do not overwrite it here: doing so would silently
    # downgrade a quadratic Tet10 design to a linear Tet4 analysis model.

    # ====================================================================================
    # 3. 材料与截面
    # ====================================================================================
    material_properties() # 定义材料属性(# 在mdl中创建 Material-A / Material-B)
    # 第一个部件的截面
    mdl.HomogeneousSolidSection(name='Section-A', material='Material-A', thickness=None) # 材料A的均质实体截面，厚度参数不适用（仅用于壳单元）
    p_a.SectionAssignment(
        region=p_a.sets['Set-ALL'],
        sectionName='Section-A',
        offset=0.0,
        offsetType=MIDDLE_SURFACE,
        offsetField='',
        thicknessAssignment=FROM_SECTION
    )
    # 第二个部件的截面
    mdl.HomogeneousSolidSection(name='Section-B', material='Material-B', thickness=None) # 材料B的均质实体截面，厚度参数不适用（仅用于壳单元）
    p_b.SectionAssignment(
        region=p_b.sets['Set-ALL'],
        sectionName='Section-B',
        offset=0.0,
        offsetType=MIDDLE_SURFACE,
        offsetField='',
        thicknessAssignment=FROM_SECTION
    )
    print('>>> 材料属性已指定...')

    # ====================================================================================
    # 4. 装配与 PBC（周期边界条件）前的准备
    # ====================================================================================
    asm = mdl.rootAssembly
    asm.DatumCsysByDefault(CARTESIAN) # 定义装配的默认坐标系为笛卡尔坐标系
    asm.Instance(name='Part-A-1', part=p_a, dependent=ON) # 将第一个部件实例化到装配中，实例名为 Part-A-1，dependent=ON表示实例与原始部件相关联（即修改部件会影响实例）
    asm.Instance(name='Part-B-1', part=p_b, dependent=ON) # 将第二个部件实例化到装配中，实例名为 Part-B-1，dependent=ON表示实例与原始部件相关联

    print('==========================================================')
    print('>>>>>>>>>>>>>> 互锁结构的PBC <<<<<<<<<<<<<<<<<<<<<<<<<')
    print('==========================================================')

    inst_name = list(asm.instances.keys())    # 装配中实例名列表
    Num_inst = len(inst_name)           # 实例数量（2）
    nde_set = []        # 每个实例的节点集（asm）
    Num_nde = []        # 每个实例的节点数（inst）

    # 主表面节点的映射节点 [m_s_f, m_s_t, m_s_l]
    MapNdeSet = [[], [], []]
    # 表面节点集 [[s_f, s_k], [s_t, s_b], [s_l, s_r]]
    SrfNdeSet = [[[], []], [[], []], [[], []]]

    # 下面这几个变量目前在主逻辑中基本未实际使用，先保留以备后续可能的功能扩展
    EdgNde = []     # 边界节点集
    nde_e = []
    EdgSet = []
    EdgSetName = []
    TensNde = []    # 拉伸节点集
    FixNde = []     # 固定节点集

    Coord = np.empty(shape=(0, 3))  # 装配中所有节点的坐标矩阵 (asm)

    # 提取所有实例的节点坐标，后面用于整体边界识别与 PBC 映射
    for j in range(Num_inst):
        nde_set.append(asm.instances[inst_name[j]].nodes)   # 获取当前实例的节点对象列表，并添加到nde_set中
        Num_nde.append(len(nde_set[j]))                     # 获取当前实例的节点数量，并添加到Num_nde中
        for i in nde_set[j]:
            Coord = np.vstack([Coord, np.asarray(i.coordinates)]) # 将当前节点的坐标添加到Coord矩阵中，最终得到装配中所有节点的坐标矩阵
    # Coord_lmt[0,:] = 各方向最大值；Coord_lmt[1,:] = 各方向最小值
    Coord_lmt = np.vstack([np.amax(Coord, axis=0), np.amin(Coord, axis=0)])
    size = Coord_lmt[0, :] - Coord_lmt[1, :] # 模型在各个方向上的尺寸（最大值-最小值）

    # ====================================================================================
    # 5. 创建参考点（Reference Points）
    # ====================================================================================
    # RP-PIN：用于抑制整体刚体漂移和刚体转动(pin)
    # RP-NRM：用于施加法向位移边界条件（normal）
    # RP-SHR：用于 PBC 中的剪切/横向参考（shear）

    asm.deleteSets(setNames=tuple(asm.allSets.keys()))  # 删除可能已存在的装配级集合，避免重名

    for rp_name in ['RP-1', 'RP-nrm', 'RP-shr', 'RP-pin']:
        if rp_name in asm.features.keys():
            del asm.features[rp_name]

    # 装配中心点：用于固定整体刚性漂移
    rp_pin = asm.ReferencePoint(point=tuple((Coord_lmt[0, :] + Coord_lmt[1, :]) / 2))    # 用于固定刚体平移的RP
    asm.features.changeKey(fromName='RP-1', toName='RP-pin')
    asm.Set(referencePoints=(asm.referencePoints[rp_pin.id],), name='RP-PIN')
    # 位于模型外部一点：用于施加法向位移
    rp_nrm = asm.ReferencePoint(point=tuple(Coord_lmt[1, :] - size * 0.025))             # 用于法向拉伸的RP
    asm.features.changeKey(fromName='RP-1', toName='RP-nrm')
    asm.Set(referencePoints=(asm.referencePoints[rp_nrm.id],), name='RP-NRM')
    #  位于更外部一点：用于 PBC 中的剪切/横向参考
    rp_shr = asm.ReferencePoint(point=tuple(Coord_lmt[1, :] - size * 0.05))              # 用于剪切变形的RP
    asm.features.changeKey(fromName='RP-1', toName='RP-shr')
    asm.Set(referencePoints=(asm.referencePoints[rp_shr.id],), name='RP-SHR')

    # ====================================================================================
    # 6. 创建装配级节点集：区分内部节点 / 边界节点 / 固定端 / 拉伸端 / PBC 对应面节点
    # ====================================================================================
    Dim = 3 # 模型维度

    print('>>> 正在创建节点集...')
    # 创建节点集 (3D) ---------------------------------------------------------------
    k = 0 # 不同实例在 Coord 拼接数组中的起始偏移
    for j in range(Num_inst):
        for d_s in range(Dim):
            for s in [0, 1]:
                SrfNdeSet[d_s][s].append([])  # 为当前实例创建子列表
        n = asm.instances[inst_name[j]].nodes
        for i in range(int(Num_nde[j])):
            sn = i      # 节点序号（从0开始）
            lb = i + 1  #Abaqus 节点标签（从1 开始）

            # bcon: [x边界, y边界, z边界]，其中边界值为1（接近上限）或-1（接近下限），非边界为0
            bcon = identifunc(Coord_lmt[:, :Dim], Coord[i+k, :Dim], T_s)
            if (bcon == [0, 0, 0]).all():
                continue  # 忽略所有内部节点
            else:
                # 识别固定端和拉伸端节点；
                # 这里主要看 z 方向，同时排除某些角点/边点的冗余干扰
                if bcon[2] == -1 and bcon[0] != 1 and bcon[1] != 1: # 固定节点
                    FixNde.append(lb)
                elif bcon[2] == 1 and bcon[0] != 1 and bcon[1] != 1:# 拉伸节点
                    TensNde.append(lb)

                # 只对 x/y 周期边界上的节点建立单节点集合和映射约束
                if abs(bcon[0]) == 1 or abs(bcon[1]) == 1:  # x/ y向边界节点
                    # 创建单节点集合
                    nde = n.sequenceFromLabels(labels=(lb,), )
                    asm.Set(nodes=nde, name='N' + str(j + 1) + '-' + str(lb))

                    # 将节点分类到各方向正/负面节点集
                    for d in range(Dim):
                        if (bcon == [1, -1, 1]).all() and d == 0:  # 去除顶点节点的冗余约束
                            continue  # 从前表面移除 V_3
                        elif (bcon == [1, 1, 1]).all() and d == 0:
                            continue  # 从前表面移除 V_0
                        elif (bcon == [1, 1, -1]).all() and d == 0:
                            continue  # 从前表面移除 V_4
                        elif (bcon == [1, 1, 0]).all() and d == 0:  # 去除边节点的冗余约束
                            continue  # 从前表面移除 E_Z0
                        elif bcon[d] != 0:
                            SrfNdeSet[d][int((1-bcon[d])/2)][j].append(sn)  # 将节点放入对应表面集合
        k += Num_nde[j] # 更新下一个实例在Coord拼接数组中的起始偏移

    # 创建固定节点集与拉伸节点集
    n_a, n_b = asm.instances['Part-A-1'].nodes, asm.instances['Part-B-1'].nodes
    nde_t = n_b.sequenceFromLabels(labels=tuple(TensNde), ) # 拉伸节点集
    nde_f = n_a.sequenceFromLabels(labels=tuple(FixNde), )  # 固定节点集
    asm.Set(nodes=nde_t, name='TensNde')
    asm.Set(nodes=nde_f, name='FixNde')

    # ====================================================================================
    # 7. 查找 PBC 对应节点并建立映射关系
    # ====================================================================================
    print('>>> 正在查找耦合节点及映射关系...')
    k = 0
    for j in range(Num_inst):
        # 这里只对前两个方向（通常是 x、y）做周期映射
        for d_s in range(Dim-1):
            map_nde = nns(
                Coord[k:k + Num_nde[j], :],
                SrfNdeSet[d_s][0][j],
                SrfNdeSet[d_s][1][j],
                Dim,
                d_s
            )
            MapNdeSet[d_s].append(map_nde)
        k += Num_nde[j]

    # ====================================================================================
    # 8. 施加 PBC 方程约束
    # ====================================================================================
    print('>>> Applying Equation Constraints...')           # 应用方程约束
    mdl.constraints.delete(tuple(mdl.constraints.keys()))   # 删除之前的约束
    Dir_s = ['f', 't', 'l']           # 用于命名：front / top / left
    Dir_c = ['x', 'y', 'z']           # 位移分量名
    SetRP = ['RP-SHR', 'RP-NRM']      # PBC 方程中用到的参考点集名
    # PBC约束方程 -------------------------------------------------------
    for d_s in range(Dim - 1):  # 周期方向
        k = 0
        for j in range(Num_inst):  # 实例序号
            for i in range(len(SrfNdeSet[d_s][0][j])):  # 节点序号
                idx_mst = SrfNdeSet[d_s][0][j][i]           # 主节点索引
                idx_slv = list(map(int, MapNdeSet[d_s][j][i, :])) # 从节点索引
                idx_row = np.insert(idx_slv, 0, idx_mst)

                SetMst = 'N' + str(j + 1) + '-' + str(idx_mst + 1) # 主节点集合名

                # 取主节点和三个从节点的坐标，去掉当前周期方向后，用于求映射权重 W
                Mtx_coord = Coord[idx_row + k, :]
                Mtx_coord = np.delete(Mtx_coord, d_s, axis=1)
                W = mapping(Mtx_coord, T_v)

                for d_c in range(Dim):  # 约束的位移分量
                    CstrName = 'Eqn-' + Dir_s[d_s] + str(j + 1) + '-' + str(idx_mst + 1) + Dir_c[d_c] # 约束名称
                    SetSlv = 'N' + str(j + 1) + '-' # 从节点集合名的前缀
                    SetSlv0 = SetSlv + str(idx_slv[0] + 1)
                    SetSlv1 = SetSlv + str(idx_slv[1] + 1)
                    SetSlv2 = SetSlv + str(idx_slv[2] + 1)

                    # 参考点RP约束方向：如果约束的位移分量与周期方向相同，则用剪切RP，否则用法向RP
                    if d_s == d_c:
                        Dir_c_RP = d_c + 1   # 对应RP-SHR的位移分量
                    else:
                        Dir_c_RP = d_s + d_c # 对应RP-NRM的位移分量

                    # 约束方程项：主节点位移 - 从节点位移的加权平均 = 参考点位移/0
                    EqnTerms = ((1.0, SetMst, d_c + 1), (-1, SetRP[d_s == d_c], Dir_c_RP),
                                (-W[0], SetSlv0, d_c + 1), (-W[1], SetSlv1, d_c + 1), (-W[2], SetSlv2, d_c + 1))
                    mdl.Equation(name=CstrName, terms=EqnTerms) # 创建方程约束
            k += Num_nde[j]

    # 拉伸约束方程 -------------------------------------------------------
    mdl.Equation(name='Eqn-Tens', terms=((1.0, 'TensNde', 3), (-1.0, 'RP-NRM', 3)))

    #  后续边界条件中用到的region句柄
    rgn_f = asm.sets['FixNde']  # 固定端节点集（fix）
    rgn_t = asm.sets['TensNde'] # 拉伸端节点集（tensile）
    rgn_p = asm.sets['RP-PIN']  # 刚体约束参考点（pin）
    rgn_n = asm.sets['RP-NRM']  # 法向位移参考点（normal）


    # ====================================================================================
    # 9. 分析步与输出请求
    # ====================================================================================
    print('>>> 正在创建步骤...')

    # 删除除 Initial 以外的旧分析步
    for stp_name in list(mdl.steps.keys()):
        if stp_name == 'Initial':
            continue
        else:
            del mdl.steps[stp_name]

    # 创建静力分析步
    step_config = SIM_CONFIG.get('step', {})
    mdl.StaticStep(
        name='Step-1',
        previous='Initial',
        timePeriod=StepTime,
        stabilizationMagnitude=float(step_config.get('stabilization_magnitude', 0.0002)),
        stabilizationMethod=DISSIPATED_ENERGY_FRACTION,
        continueDampingFactors=False,
        adaptiveDampingRatio=float(step_config.get('adaptive_damping_ratio', 0.05)),
        maxNumInc=int(step_config.get('max_num_inc', 500)),
        initialInc=float(step_config.get('initial_inc', 0.001)),
        minInc=float(step_config.get('min_inc', 1e-5)),
        maxInc=float(step_config.get('max_inc', 0.1)),
        nlgeom=ON
    )


    # 场输出：应力、应变、位移、反力、接触应力、接触位移、单元体积等
    # S        应力，单元积分点，多分量
    # PE       塑性应变，多分量
    # PEEQ     等效塑性应变
    # PEMAG    塑性应变幅值
    # LE       对数应变，多分量
    # U        节点位移
    # RF       节点反力
    # CSTRESS  接触应力
    # CDISP    接触位移
    # EVOL     单元体积
    # STATUS   单元状态
    mdl.fieldOutputRequests['F-Output-1'].setValues(
        variables=('U', 'S', 'PEEQ')
    )

    # 历史输出：记录 RP-NRM 上的位移和反力，用于后续等效应力-应变计算
    mdl.HistoryOutputRequest(
        name='H-Output-1',
        createStepName='Step-1',
        variables=('U1', 'U2', 'U3', 'RF3',), # 主要监控法向位移和反力
        region=rgn_n,                         # 输出区域为法向位移参考点
        sectionPoints=DEFAULT,
        rebar=EXCLUDE
    )

    # ====================================================================================
    # 10. 接触 / 边界条件 / 耦合约束
    # ====================================================================================
    print('>>> 正在应用接触/边界条件/耦合约束...')

    #--- 接触 ---
    mdl.ContactProperty('IntProp-1') # 创建接触属性
    contact_config = SIM_CONFIG.get('contact', {})
    if str(contact_config.get('formulation', 'frictionless')).lower() == 'penalty':
        friction_coefficient = float(contact_config.get('friction_coefficient', 0.0))
        mdl.interactionProperties['IntProp-1'].TangentialBehavior(
            formulation=PENALTY, table=((friction_coefficient,),)
        )
    else:
        mdl.interactionProperties['IntProp-1'].TangentialBehavior(formulation=FRICTIONLESS)
    rgn_m = asm.instances['Part-A-1'].surfaces['Surf-Cont'] # 主表面：Part-A-1 的接触面
    rgn_s = asm.instances['Part-B-1'].surfaces['Surf-Cont'] # 从表面：Part-B-1 的接触面

    adjust_name = str(contact_config.get('adjust_method', 'overclosed')).lower()
    adjust_constants = {'none': NONE, 'overclosed': OVERCLOSED, 'tolerance': TOLERANCE}
    if adjust_name not in adjust_constants:
        raise ValueError('Unsupported contact adjust method: %s' % adjust_name)

    contact_kwargs = dict(
        name='Int-Cont',                    # 接触名称
        createStepName='Initial',           # 在哪个分析步创建接触
        sliding=FINITE if str(contact_config.get('sliding', 'small')).lower() == 'finite' else SMALL,
        thickness=ON,                       # 面厚度：开启
        interactionProperty='IntProp-1',    # 接触属性
        surfaceSmoothing=AUTOMATIC,         # 曲面光滑处理
        adjustMethod=adjust_constants[adjust_name],
        initialClearance=OMIT,              # 忽略初始间隙
        datumAxis=None,                     # 基准轴（无）
        clearanceRegion=None                # 间隙区域（无）
    )
    if adjust_name == 'tolerance':
        contact_kwargs['adjustTolerance'] = float(contact_config.get('adjust_tolerance', 0.01))
    try:
        mdl.SurfaceToSurfaceContactStd(main=rgn_m, secondary=rgn_s, **contact_kwargs)
    except TypeError:
        mdl.SurfaceToSurfaceContactStd(master=rgn_m, slave=rgn_s, **contact_kwargs)

    # --- 固定端边界条件 ---
    mdl.ZsymmBC(name='BC-Fix', createStepName='Initial', region=rgn_f, localCsys=None)

    # --- 拉伸边界条件 ---
    mdl.TabularAmplitude(name='Amp-1', timeSpan=STEP, smooth=SOLVER_DEFAULT, data=((0.0, 0.0), (StepTime, 1.0))) # 幅值曲线：线性加载
    mdl.DisplacementBC(name='BC-Tens', createStepName='Step-1', region=rgn_n,
                    u1=UNSET, u2=UNSET, u3=u_tens, ur1=0.0, ur2=0.0, ur3=0.0, amplitude='Amp-1', fixed=OFF,
                    distributionType=UNIFORM, fieldName='', localCsys=None) # 位移拉伸

    # 固定RP-PIN点约束
    mdl.DisplacementBC(name='BC-Pin', createStepName='Initial', region=rgn_p,
                    u1=SET, u2=SET, u3=UNSET, ur1=SET, ur2=SET, ur3=SET, amplitude=UNSET,
                    distributionType=UNIFORM, fieldName='', localCsys=None)

    # 耦合RP-PIN与固定端（使固定端节点完全跟着RP-PIN点动）
    mdl.Coupling(name='Cpl-Pin', controlPoint=rgn_p, surface=rgn_f, influenceRadius=WHOLE_SURFACE,
                couplingType=DISTRIBUTING, weightingMethod=UNIFORM, localCsys=None,
                u1=ON, u2=ON, u3=ON, ur1=ON, ur2=ON, ur3=ON)

    progress('Pre-processing finished.')


    # ====================================================================================
    # 11. 建立并提交作业
    # ====================================================================================
    num_cpus = int(SIM_CONFIG.get('num_cpus', 10))
    memory_percent = int(SIM_CONFIG.get('memory_percent', 90))
    scratch_dir = str(SIM_CONFIG.get('scratch_dir', ''))
    if scratch_dir and not os.path.isdir(scratch_dir):
        os.makedirs(scratch_dir)
    mdb.Job(
            name=job_name,                  # 作业名称
            model=model_name,               # 模型名称
            description='',
            type=ANALYSIS,
            atTime=None,
            waitMinutes=0,
            waitHours=0,
            queue=None,
            memory=memory_percent,
            memoryUnits=PERCENTAGE,
            getMemoryFromAnalysis=True,
            explicitPrecision=SINGLE,
            nodalOutputPrecision=SINGLE,
            echoPrint=OFF,
            modelPrint=OFF,
            contactPrint=OFF,
            historyPrint=OFF,
            userSubroutine='',
            scratch=scratch_dir,
            resultsFormat=ODB,              # 输出结果文件格式
            multiprocessingMode=DEFAULT,
            numCpus=num_cpus,
            numDomains=num_cpus,
            numGPUs=0
        )

    if not bool(SIM_CONFIG.get('submit_job', True)):
        mdb.jobs[job_name].writeInput(consistencyChecking=OFF)
        progress('Preprocess-only validation complete; input file written without job submission.')
        sys.exit(0)

    mdb.jobs[job_name].submit(consistencyChecking=OFF)
    progress('Abaqus job submitted; waiting for STA progress.')

    # ====================================================================================
    # 12. 求解监控：自动识别 0.2% 偏移证明应力点并终止计算
    # ====================================================================================

    if postprc == 1:
        # PrePrc 只负责前处理、提交和监控求解；ODB 后处理统一交给 TPMS11_PostODB.py。
        while True:
            if abaqus_job_completed(work_dir, job_name):
                if os.path.exists(lck_filepath):
                    try:
                        os.remove(lck_filepath)
                    except Exception:
                        pass
                if not os.path.exists(odb_filepath):
                    raise RuntimeError(
                        'Abaqus job completed, but ODB file was not found: %s' % odb_filepath
                    )
                progress('Abaqus job completed; ODB is ready for postprocess.')
                sys.exit(0)

            failed, fail_file, fail_pattern = abaqus_job_failed(work_dir, job_name)
            if failed:
                progress('Abaqus job failed before ODB postprocess: %s (%s)' % (fail_file, fail_pattern))
                report_small_volume_elements(work_dir, job_name)
                raise RuntimeError('Abaqus job failed. Check %s for details.' % fail_file)

            time.sleep(5)

    # 求两条线段的交点函数
    def get_line_intersection(p1, p2, p3, p4):
        """
        求两条线段 (p1, p2) 和 (p3, p4) 的交点。
        返回交点 (x, y)，或在无交点时返回 None。
        """
        x1, y1 = p1
        x2, y2 = p2
        x3, y3 = p3
        x4, y4 = p4

        denom = (y4 - y3) * (x2 - x1) - (x4 - x3) * (y2 - y1) # 计算分母
        if denom == 0:
            return None  # 平行或共线

        ua = ((x4 - x3) * (y1 - y3) - (y4 - y3) * (x1 - x3)) / denom
        ub = ((x2 - x1) * (y1 - y3) - (y2 - y1) * (x1 - x3)) / denom

        if 0 <= ua <= 1 and 0 <= ub <= 1:
            x = x1 + ua * (x2 - x1)
            y = y1 + ua * (y2 - y1)
            return (x, y)

        return None

    # 求两条折线的交点函数
    def find_intersections(A, B):
        """
        找到两条折线 A 和 B 的所有交点。
        A 和 B 是点的列表 [(x1, y1), (x2, y2), ...]
        返回交点列表 [(x, y), ...]
        """
        intersections = []

        for i in range(len(A) - 1, 0, -1):
            p1, p2 = A[i - 1], A[i]
            p3, p4 = B[i - 1], B[i]

            intersection = get_line_intersection(p1, p2, p3, p4)
            if intersection:
                intersections.append(intersection)

        return intersections

    if postprc == 1:
        inc_thresh = 10

        while True:
            # ----------------------------------------------------------
            # 循环 1：持续监控，直到检测到等效0.2%偏移屈服应力点为止
            # ----------------------------------------------------------
            while True:
                # ------------------------------------------------------
                # 循环 2：等待 STA 文件中的有效增量达到阈值
                # ------------------------------------------------------
                while True:
                    # --------------------------------------------------
                    # 循环 3：等待 STA 文件更新出新的有效增量
                    # --------------------------------------------------
                    valid_increment_updated = False
                    # 检查有效增量
                    if sta_file in os.listdir(work_dir):  # 检测到 STA 文件
                        with open(sta_file, 'r', encoding='latin-1', errors='ignore') as file:
                            sta_lines = file.readlines()
                            if sta_lines:
                                last_line = sta_lines[-1].strip().split()  # 读取 STA 文件最后一行
                                if len(last_line) > 2 and last_line[2].isdigit():  # 有效增量已更新
                                    valid_increment_updated = True
                    # 中断循环 3
                    if valid_increment_updated: # 有效增量已更新
                        break
                    if abaqus_job_completed(work_dir, job_name):
                        if not os.path.exists(odb_filepath):
                            raise RuntimeError(
                                'Abaqus job completed, but ODB file was not found: %s' % odb_filepath
                            )
                        progress('Abaqus job completed before readable STA progress; read final ODB.')
                        last_line = ['1', str(inc_thresh), '1']
                        break
                    failed, fail_file, fail_pattern = abaqus_job_failed(work_dir, job_name)
                    if failed:
                        progress('Abaqus job failed before STA progress was available: %s (%s)' % (fail_file, fail_pattern))
                        report_small_volume_elements(work_dir, job_name)
                        raise RuntimeError('Abaqus job failed. Check %s for details.' % fail_file)
                    time.sleep(5)
                # 中断循环 2
                if int(last_line[1]) >= inc_thresh: # 有效增量达到阈值
                    break

            # 若存在lck文件则删除，确保ODB可读取
            if os.path.exists(lck_filepath):
                os.remove(lck_filepath)

            # 打开并读取 ODB 文件中 RP-NRM点的历史输出
            odb = odbAccess.openOdb(r'C:\\temp\\' + job_name + '.odb')  # ,readOnly=True
            ho = odb.steps['Step-1'].historyRegions['Node ASSEMBLY.2'].historyOutputs
            Force3 = [row[1] for row in ho['RF3'].data]
            Disp1 = [row[1] for row in ho['U1'].data]
            Disp2 = [row[1] for row in ho['U2'].data]
            Disp3 = [row[1] for row in ho['U3'].data]

            # ----------------------------------------------------------
            # 根据 RP 反力/位移构造等效应力-应变曲线
            # ----------------------------------------------------------
            Stress_eq = [f3 / ((ucs + u1) * (ucs + u2)) for f3, u1, u2 in zip(Force3, Disp1, Disp2)]  # 等效应力
            Strain_eq = [u3 / thk_l for u3 in Disp3]  # 等效应变

            # 用前期近线性段估算等效弹性模量
            E_eq = Stress_eq[1] / Strain_eq[1]  # 等效弹性模量
            Curve1 = list(zip(Strain_eq, Stress_eq)) # 等效应力-应变曲线

            # 构造 0.2% 偏移线：sigma = E * (eps - 0.002)
            Stress_ofst = [E_eq * (strain - 2e-3) for strain in Strain_eq]  # 偏移应力
            Curve2 = list(zip(Strain_eq, Stress_ofst)) # 0.2% 偏移线

            intersections = find_intersections(Curve1, Curve2) # 查找交点

            if len(intersections) == 0:
                if abaqus_job_completed(work_dir, job_name):
                    progress("Analysis completed but no proof stress intersection was found; write penalty result=0.")
                    with open('result.txt', 'w') as result_file:
                        result_file.write("0.000\n")
                    write_penalty_result_to_csv()
                    break
                # print(">>> 找不到交点...")
            else:
                if not abaqus_job_completed(work_dir, job_name):
                    os.system('abaqus terminate job=' + job_name)
                progress("Proof stress point reached, Job terminated...")
                strain_proof, stress_proof = intersections[0]
                progress("Equivalent Proof Strain = %.3f %%..." % (strain_proof*100)) # 输出0.2%偏移屈服应变
                progress("Equivalent Proof Stress = %.3f MPa..." % stress_proof)      # 输出0.2%偏移屈服应力
                progress(">>>>>>>>>>>> %.3f, %.3f" % (stress_proof, strain_proof * 100))  # 输出应力和应变（百分比格式）

                # 将结果写入文本文件和 CSV 文件
                # 1.将stress_proof写入结果txt文件
                with open('result.txt', 'w') as result_file:
                    result_file.write("{:.3f}\n".format(stress_proof))

                progress("==========================================================")
                # 2.将stress_proof和strain_proof写入CSV文件的最后一行
                script_path = next((arg for arg in sys.argv if str(arg).lower().endswith('.py')), '')
                script_dir = os.path.dirname(os.path.abspath(script_path)) if script_path else os.getcwd()
                file_path = os.path.abspath(os.path.join(script_dir, '..', 'data', 'hzc_guess.csv'))
                with open(file_path, 'r') as file:
                    rows = list(csv.reader(file))
                    rows[-1][4] = '{:.3f}'.format(-stress_proof)       # 将屈服强度写入第5列（索引4）
                    rows[-1][5] = '{:.3f}'.format(strain_proof * 100)  # 将屈服应变写入第6列（索引5）
                with open(file_path, 'w', newline='') as file:
                    csv.writer(file).writerows(rows)
                fill_latest_csv_offset_from_metadata(file_path, mesh_path, progress=progress)

                break
