# 产品官网与用户云端控制台（2026-10-20）

## 结论

- `/` 是公开产品官网，不再承载注册表单。
- `/console` 与 `/register` 是普通用户云端入口。
- `/admin` 与兼容路径 `/account` 是项目管理员后台。
- 普通用户账户使用独立的 `cloud_accounts` / `cloud_sessions`，不会获得管理员、固件或 AI 服务密钥权限。
- 官网只公开 `stable + published` 的项目固件；设备 OTA 的原有鉴权下载保持不变。

## 视觉与动效

官网最终采用“真实硬件工作室 / 产品档案”方向：自然纸张底色、普通无衬线字体、实物原型作为首屏主体，不再使用黑底霓虹、超大衬线字、英文编号章节和跑马灯等容易产生模板化 AI 感的形式。

最终首屏不再混用实物照片与代码表情：改为 ImageGen 根据真实原型生成的正面、对称、透明底品牌插画，并单独生成无表情的黑色屏幕底图。网页 Canvas 只覆盖屏幕内容，因此机身、材质与表情动效不会互相漂移。

首页表情支持安静、好奇、开心、思考、困倦五种自动状态，并尊重 `prefers-reduced-motion`。鼠标进入机器人展示区时自动表情轮播暂停并切回中性脸，只有双眼跟随指针；机身、嘴巴和表情造型保持不动。鼠标离开后双眼归中并恢复此前表情。旧的独立表情试玩区已删除，避免一个首页重复出现两张脸。

首页叙事改为项目本身：当前工作重点是独立固件、Android App 与云端体验；自研外观、结构件和扩展模块明确标注为后续计划。硬件来源区明确说明当前原型基于 ESP-Friends 发布的 ESP-SparkBot 开源硬件方案，硬件工程公开在立创开源硬件平台，头部与底盘分别使用乐鑫 ESP32-S3 / ESP32-C2；Baby Tank 只参考公开硬件资料、引脚和底盘协议，不以上游示例或小智应用作为代码基线。页面同时保留非背书声明，避免把芯片厂商、平台和硬件发布方混为同一主体。

设计参考只吸收方法，不复制组件：

- Bruno Simon `folio-2019`：先通过互动建立角色认知，再解释功能。
- Codrops 开源演示：使用滚动节奏组织连续叙事。
- `leisurelyleon/aurora`：限制 DPR、离屏/弱设备降级、reduced-motion 等性能原则。

## 云端接口

- `POST /api/cloud/v1/register`
- `POST /api/cloud/v1/login`
- `GET /api/cloud/v1/me`
- `POST /api/cloud/v1/logout`
- `GET /api/public/v1/releases`
- `GET /api/public/v1/releases/:id/download`

注册限制：邮箱唯一、密码 12–128 字符、成年人/监护人确认、条款确认、蜜罐字段、IP 速率限制。会话 cookie 为 HttpOnly + SameSite=Strict，退出需要 CSRF token。

## 当前边界

账户注册与会话隔离已经可用；账户与设备绑定、每个账户独立的 AI Provider 配置尚未落地。因此控制台当前只展示“未绑定设备”，后续必须在 `cloud_accounts` 与设备/家长授权之间增加明确的所有权关系，不能复用管理员的全局 Provider 配置。

邮件验证与找回密码也尚未接入。正式开放给外部用户前，需要补齐邮件发送、验证令牌、密码重置、隐私政策版本记录和账号删除流程。

## 生产发布

- 服务：`/opt/baby-tank-cloud`
- 发布前备份：`/opt/baby-tank-cloud/backups/site-cloud-20261020-1611/app-and-data.tgz`
- 新官网发布前静态文件备份：`/opt/baby-tank-cloud/backups/site-redesign-20261020-1645`
- 正面插画精简版发布前备份：`/opt/baby-tank-cloud/backups/site-front-20261020-1725`
- 动态屏幕与项目定位版发布前备份：`/opt/baby-tank-cloud/backups/site-dynamic-face-20261020-1748`
- 稳定固件：`0.14.31`
- 类型：头部应用分区镜像（不是整机工厂镜像）
- 大小：`3,285,984 bytes`
- SHA-256：`ebf8ebf6eb37b7f38fb12a64e276c172bf1a48dbcc398e3990410ac7f2bca9f2`
- 验证：健康检查、公开版本列表、完整下载、Range 下载、线上/本地 SHA-256 一致。

本次部署只合入网站、云端账户和公开固件接口，没有把本地尚未整体发布的心情/人格分支一起覆盖到生产。

## 管理员认证说明

- 普通用户入口 `/console` 使用邮箱和独立的 `bt_cloud_session`。
- 项目管理员入口 `/admin` 使用管理员名和独立的 `bt_session`，当前管理员名为 `admin`，不要求邮箱格式。
- 2026-10-20 部署时发现生产 `.env` 缺失 `ADMIN_PASSWORD`，导致数据库中的密码哈希与运行配置不一致。修复前已备份数据库到 `/opt/baby-tank-cloud/backups/admin-auth-20261020-163056/baby-tank.sqlite3`，随后补齐配置、同步哈希并清除旧管理员会话。
- 修复后已通过服务内部真实登录请求验证，返回 HTTP 200。临时密码不写入项目文档或仓库。
