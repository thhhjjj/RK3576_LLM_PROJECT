# PHASE4 已做记录

> 对照 [PHASE4.md](./PHASE4.md)。

## 结论

**本阶段取消，不做。**

BLE GATT / WebBLE 对当前产品路径鸡肋：远处聊天走 WebSocket（+ 可选 frpc）即可；近场 BLE 收益小、联调成本高。扇出里 BLE 订阅者继续留空接口，不实现。

公网通路改走 [PHASE5.md](./PHASE5.md) **frpc**（默认关、UI 起停）。
