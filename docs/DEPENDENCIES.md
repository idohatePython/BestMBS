# 构建依赖

仓库不提交 `third_party` 和 VTK 构建目录。这样可以避免 Git 历史被数百 MB 的上游
源码与二进制产物占满，同时也让各依赖的来源和许可证保持清楚。

## 已验证版本

| 依赖 | 版本 | 用途 | 获取方式 |
|---|---:|---|---|
| CMake | 3.25+ | 配置、构建、安装 | 系统安装 |
| Qt Desktop / MinGW | Qt 6.11.2 / GCC 13.1 | GUI 与 Windows 工具链 | Qt Online Installer |
| SQLite | 3.x | 持久化 | MinGW 开发包或独立前缀 |
| Boost | 1.8x+ | CGAL 精确数后端 | 系统安装或独立前缀 |
| Manifold | 3.5.2 | 隐式面与布尔 | 准备脚本 |
| CGAL | 6.0.3 | 网格诊断、修复与重网格 | 准备脚本 |
| Eigen | 3.4.0 | CGAL 自适应尺寸场 | 准备脚本 |
| TetGen | 1.6.0 | 四面体化 | 准备脚本并应用补丁 |
| VTK | 9.5.2 | 三维视口与 VTK I/O | 单独构建脚本 |
| Abaqus/CAE | 2025 | 求解与 ODB 后处理 | 可选，用户自行安装 |

## 本地目录布局

执行 `tools/bootstrap_dependencies.ps1` 后会得到：

```text
third_party/
  cgal/
  eigen/
  manifold/
  tetgen/
```

执行 `tools/bootstrap_vtk.ps1` 后还会生成：

```text
third_party/vtk/
  source/
  build/
  install/
```

这些目录全部被 Git 忽略。脚本固定到项目已验证的上游提交，并为 Tet10 生命周期
问题以及 VTK/MinGW 兼容问题应用仓库内的小型补丁。

## CMake 路径覆盖

如果依赖已经安装在其他位置，不必复制到 `third_party`。可以设置下列 CMake 缓存项：

- `MBS_MANIFOLD_DIR`
- `MBS_TETGEN_DIR`
- `MBS_CGAL_DIR`
- `MBS_EIGEN_DIR`
- `MBS_VTK_DIR`
- `MBS_SQLITE_ROOT`

主要开关：

- `MBS_BUILD_GUI=ON/OFF`
- `MBS_BUILD_TESTS=ON/OFF`
- `MBS_ENABLE_CGAL=ON/OFF`
- `MBS_ENABLE_TETGEN=ON/OFF`

关闭 TetGen 可构建不带体网格功能的版本，但完整桌面版默认启用 TetGen。

## Abaqus

Abaqus 不参与 C++ 编译。程序运行时查找 `MBS_ABAQUS_COMMAND`，也会尝试使用系统
`PATH` 中的 `abaqus` 命令。没有 Abaqus 时，结构设计、网格、数据库和优化功能仍可
使用，只有前处理、求解、ODB 后处理和动画导出不可用。
