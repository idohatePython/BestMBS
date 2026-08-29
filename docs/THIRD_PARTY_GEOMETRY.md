# Third-party notices

BestMBS 的完整构建使用下列第三方项目。它们不包含在本仓库中，下载后仍受各自的
许可证与版权声明约束。

| 项目 | 固定版本 | 许可证 | 在 BestMBS 中的用途 |
|---|---|---|---|
| Manifold | 3.5.2 (`11235e6`) | Apache-2.0 | LevelSet、流形表示和布尔运算 |
| CGAL | 6.0.3 (`cefe300`) | 包级 GPL-3.0-or-later / LGPL-3.0-or-later，另有商业许可 | PMP 诊断、修复、重网格与简化 |
| Eigen | 3.4.0 | MPL-2.0（构建定义 `EIGEN_MPL2_ONLY`） | CGAL 自适应尺寸场 |
| TetGen | 1.6.0 (`e05aca7`) | AGPL-3.0-or-later，另有商业许可 | Tet4/Tet10 四面体化 |
| VTK | 9.5.2 | BSD-3-Clause | 网格 I/O 与三维渲染 |
| Qt | 6.5+ | LGPL-3.0/GPL-3.0 或商业许可 | 桌面界面 |
| SQLite | 3.x | Public Domain | 本地数据库 |
| Boost | 1.8x+ | Boost Software License 1.0 | CGAL 数值支持 |

构建安装树会复制 Manifold、TetGen、CGAL 和 Eigen 的许可证文件。Qt 与 VTK 的
发行要求仍需由二进制分发者按实际链接方式履行。官方许可信息：

- <https://github.com/elalish/manifold>
- <https://www.cgal.org/license.html>
- <https://eigen.tuxfamily.org/index.php?title=Main_Page#License>
- <https://www.wias-berlin.de/software/tetgen/FAQ-license.jsp>
- <https://vtk.org/about/>
- <https://www.qt.io/licensing/open-source-lgpl-obligations>

仓库中的 `patches/` 只保存为兼容当前工具链而需要的最小差异：

- TetGen 补丁延长二阶单元中间节点连接表的生命周期，避免输出 Tet10 时访问悬空
  指针；
- VTK 补丁处理 MinGW 下 TIFF 导出声明和结构化点数组外部模板实例化问题。

这些补丁不会改变相应第三方项目的许可证。
