# Architecture

BestMBS 把桌面交互、业务规则、外部进程和计算几何分开，避免 Qt、SQLite 或 Abaqus
细节进入领域模型。

```text
apps/gui             Qt 6 桌面进程
apps/worker          隔离运行的 C++ 计算进程
include/mbs/domain   设计参数、材料、任务和工作流规则
include/mbs/application  用例与持久化端口
src/geometry         TPMS、Manifold、CGAL、TetGen 和接触风险
src/optimization     高斯过程、采集函数与约束采样
src/infrastructure   SQLite、Worker 桥接与 Abaqus Gateway
src/presentation     Qt/VTK 界面适配层
runtime/python       Abaqus CAE/ODB 脚本
```

依赖方向由外向内：Presentation 和 Infrastructure 可以依赖 Application 与 Domain，
Domain 不依赖 Qt、数据库、文件系统、VTK 或 Abaqus。`architecture-isolation` 测试会
检查这些边界。

## Worker 边界

耗时任务由 `mbs-worker` 执行，GUI 通过逐行 UTF-8 事件接收进度、制品和指标。
`WorkerSession` 校验协议版本与任务编号；`WorkerTaskBridge` 再把有效事件写入事务化的
生命周期存储。进程异常、取消和业务失败具有不同的最终状态，不把不完整目录发布为
成功样本。

## 几何流水线

几何模块依次处理隐式场、Manifold LevelSet/Boolean、CGAL 表面诊断与重网格，以及
可选的 TetGen Tet4/Tet10 体网格。每个阶段都输出可视化制品和可读质量指标。
生成目录先写入 staging，全部质量门通过后再原子发布。

## 数据与外部求解器

SQLite 仓储保存样本、材料、优化观测、任务、运行和制品元数据；大型网格、ODB 和
动画仍放在样本目录。`AbaqusGateway` 负责 noGUI 前处理、求解监控、ODB 后处理和
动画导出，Abaqus 专属 API 只存在于随程序部署的 Python 脚本中。

优化引擎是独立的 C++ 库，不依赖 Qt、SQLite、Python 或 scikit-learn。界面只读取
仓储中的观测和预测结果，不直接训练模型。
