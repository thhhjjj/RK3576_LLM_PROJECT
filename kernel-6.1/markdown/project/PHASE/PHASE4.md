# 第四阶段：做到哪一步

> 依据 [project.md](../project.md) 的 **BLE** 层。**排期上属添头**：主线优先 3 → 6 → 7；本阶段不挡主线出口。  
> 做完时：本机 `hci0` 上 GATT 可写提问、Notify 流式推 token；手机走远只丢 BLE 一路，本机屏与 WebSocket 继续。frpc、拼音输入法仍不做。

第三阶段见 [PHASE3.md](./PHASE3.md)。建议主线（云端板测、GPU、体验）告一段落再开 GATT；扇出可先留 BLE 空接口。

## 完成线（摘要）

1. Seekwave `hci0` UP；BlueZ GATT 外设：可写特征收问题，Notify 特征推 token（按 MTU 分片）。  
2. BLE 独立发送队列；写失败 / 断开只回收 BLE 路；推理与 LVGL / WS 不停。  
3. 与 PHASE3 同一 `tokq` 扇出：UI + WS + **BLE** 三路。  
4. 调试可用 nRF Connect；浏览器 WebBLE 要 HTTPS/localhost（本阶段可不挡出口）。  
5. **不做**：frpc、公网 TLS 页、拼音输入法。

## 细分步骤（开工后往 PHASE4_DID 记）

1. 确认 `hci0` / `bluetoothd` 在裁剪 rootfs 可起。  
2. 注册 GATT service + 写特征 + Notify 特征。  
3. `tokq` fan-out 增加 BLE 订阅者（同 WS：只入队，不在 RKLLM 回调里 Notify）。  
4. 手机写特征 → `panel_ask`（尊重本地/云端模式）。  
5. 断连压测：拔手机不影响屏与 WS。  
6. 收口写入 PHASE4_DID。

## 当前状态

**取消 / 跳过。** 见 [PHASE4_DID.md](./PHASE4_DID.md)。主线不挡；公网改走 [PHASE5.md](./PHASE5.md) frpc。
