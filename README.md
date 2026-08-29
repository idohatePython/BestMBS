# BestMBS 4.0

BestMBS 是一个面向机械互锁 TPMS 结构的桌面设计与优化平台。4.0 版本以 C++20
重写核心业务和几何流程，使用 Qt 6 构建界面、VTK 展示三维网格，并把结构生成、
表面修复、四面体化、数据管理、贝叶斯优化和 Abaqus 工作流串在同一个程序中。

Windows 可执行版可直接从 [Releases](https://github.com/idohatePython/BestMBS/releases)
下载。仓库保存项目源码和构建脚本，不重复存放 Release 压缩包、构建产物、个人样本
数据库或大体积第三方源码。

## 主要功能

- 四参数 TPMS 隐式场设计，支持相位、RVE 尺寸、重复数和盖板等参数；
- Manifold 等值面提取与布尔建模；
- CGAL 表面诊断、修复、均匀/曲率自适应重网格、特征保护和受限简化；
- TetGen Tet4/Tet10 四面体网格及 Abaqus C3D4/C3D10 导出；
- VTK 多对象视口，以及 Boolean、Remesh、Tetrahedralization 对比视图；
- SQLite 样本、材料、任务与制品管理；
- 基于高斯过程的贝叶斯优化；
- Abaqus 前处理、求解监控、ODB 只读后处理和 PNG 云图动画导出。

几何流程会记录边界边、连通域、非二流形边/点、孔洞、退化面、自相交、三角形
质量和表面偏差等指标。生成结果没有通过质量门时不会发布为完整样本。

## 技术栈

- C++20、CMake 3.25+
- Qt 6.5+（当前 Windows 发行版使用 Qt 6.11.2 / MinGW 13.1）
- VTK 9.5.2
- Manifold 3.5.2
- CGAL 6.0.3、Eigen 3.4.0、Boost
- TetGen 1.6.0
- SQLite 3
- Abaqus/CAE 2025（可选，仅仿真相关功能需要）

## 从源码构建

目前完整验证的环境是 Windows 11、Qt MinGW 13.1 和 CMake 的 `MinGW Makefiles`
生成器。`third_party` 不纳入 Git；准备脚本会把固定版本下载到本地：

```powershell
git clone https://github.com/idohatePython/BestMBS.git
cd BestMBS
.\tools\bootstrap_dependencies.ps1
```

VTK 需要使用与 Qt 相同的 MinGW ABI 单独编译。请把下面路径换成自己的 Qt 安装位置：

```powershell
.\tools\bootstrap_vtk.ps1 `
  -QtRoot C:\Qt\6.11.2\mingw_64 `
  -MinGWRoot C:\Qt\Tools\mingw1310_64
```

随后构建项目。CGAL 还需要可供 CMake 查找的 Boost，SQLite 可使用 MinGW 工具链中
的开发包，也可通过参数指定：

```powershell
.\tools\build.ps1 -Configuration Release `
  -QtRoot C:\Qt\6.11.2\mingw_64 `
  -MinGWRoot C:\Qt\Tools\mingw1310_64 `
  -BoostRoot C:\path\to\boost `
  -SqliteRoot C:\path\to\sqlite-prefix `
  -Jobs 4
```

加入 `-Package` 会在本地 `out` 目录生成可运行的安装树。更完整的依赖版本、目录
结构和可选构建开关见 [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)。

## 数据目录

程序默认把数据库放在系统应用数据目录，把较大的样本制品放在用户文档目录下的
`BestMBS` 文件夹。若检测到原开发环境使用的 `G:\MBS`，会继续兼容该目录。
以下环境变量可以覆盖默认位置：

- `MBS_DB_PATH`
- `MBS_ARTIFACT_ROOT`
- `MBS_STAGING_ROOT`
- `MBS_GUI_CACHE_ROOT`
- `MBS_SIMULATION_RUN_ROOT`
- `MBS_SCRATCH_ROOT`
- `MBS_TEMP_ROOT`

数据库、网格、ODB、动画和运行日志都被 `.gitignore` 排除，不应提交到公开仓库。

## 项目结构

```text
apps/       GUI 与 Worker 入口
include/    公共 C++ 接口
src/        领域、应用、基础设施、几何、优化与界面实现
runtime/    随程序部署的 Abaqus Python 脚本
tests/      CTest 回归测试
tools/      依赖准备、构建和维护脚本
patches/    第三方兼容补丁（不包含第三方源码）
docs/       架构、依赖和许可证说明
```

## 许可证

BestMBS 以 **GNU Affero General Public License v3.0 or later** 发布，完整文本见
[LICENSE.txt](LICENSE.txt)。选择 AGPL 是因为完整几何构建会直接组合 AGPL 许可的
TetGen；CGAL 中本项目使用的网格处理组件也适用 GPL 条款。

第三方项目仍分别受各自许可证约束，详情见
[docs/THIRD_PARTY_GEOMETRY.md](docs/THIRD_PARTY_GEOMETRY.md)。Abaqus 是可选的商业软件，
不随本仓库或 Release 分发。
