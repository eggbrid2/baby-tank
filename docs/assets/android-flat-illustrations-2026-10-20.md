# Android 扁平插画资源（2026-10-20）

本组插画使用内置 ImageGen 逐张生成，再缩放为 512 px PNG。正式资源位于 `android/app/src/main/res/drawable-nodpi/`，统一以 `flat_` 为前缀；旧插画暂留作回滚，不再被当前界面引用。

共同提示词：透明背景、平面 2D 儿童绘本贴纸、清晰深色轮廓、少量纯色块、青色／奶油白／珊瑚色点缀、主体居中且留白；避免 3D、黏土质感、透视、投影、文字和场景背景。各张再分别指定游戏手柄、按按钮、食物饮品、石头剪刀布、拳击手套、骰子、胜负表情或宝宝坦克头像等主体。

| 类别 | 资源文件 |
| --- | --- |
| 互动入口 | `flat_interaction_games.png`、`flat_interaction_poke.png`、`flat_interaction_feed.png`、`flat_interaction_dice.png` |
| 游戏 | `flat_game_rock.png`、`flat_game_paper.png`、`flat_game_scissors.png`、`flat_game_fight.png`、`flat_game_win.png`、`flat_game_lose.png` |
| 食物 | `flat_food_strawberry.png`、`flat_food_apple.png`、`flat_food_banana.png`、`flat_food_cake.png`、`flat_food_cookie.png`、`flat_food_bread.png`、`flat_food_rice_ball.png`、`flat_food_ice_cream.png` |
| 饮品 | `flat_drink_milk.png`、`flat_drink_juice.png`、`flat_drink_water.png`、`flat_drink_yogurt.png`、`flat_drink_soy_milk.png`、`flat_drink_cocoa.png` |
| 应用图标 | `flat_ic_launcher.png` |

首页两张扁平插画另外保存在同一目录：`icon_home_drive_flat_generated.png` 和 `icon_home_play_flat_generated.png`。摄像头实时画面和 Compose 实时绘制的表情不是静态插画，本次不替换。
