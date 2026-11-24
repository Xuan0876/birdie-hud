// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// The settings panel, v2 "pocket": the companion's voice, the HUD switch, three scales, and two pages that slide in
// (symbols, fine tuning). A preview unfolds on top only while the real HUD is not on the screen; hovering a symbol
// lights it up on the real HUD (focus message) and in the preview.
// Talks to Birdie.exe over ws://127.0.0.1 (see src/app/panel_proto.h for the messages). Every control sends its
// change at once; Birdie applies it to the HUD, saves it, and sends the new state back to every open panel.
// Inside Birdie's own window (WebView2) it also talks to the window: its height, close, drag (settings_window.h).
(function () {
  'use strict';

  var PORTS = [47820, 47821, 47822, 47823];  // src/app/app.cpp kPanelPort, kPanelPortTries
  var D = 36.5714, CX = 640, HY = 420, BORE = 329.143;  // HUD canvas: px per degree, centre, horizon y, boresight y
  var DESIGN_W = 352;  // px at 1rem = 14px
  var PREVIEW_MIN_OPACITY = 0.45;  // the preview never fades below this (the value is written next to it)

  // what the panel shows; settings mirror settings.json, status is Birdie's view of the sim
  var S = {
    online: false, lang: 'zh', focus: null, page: 0,
    set: { visible: true, glide_deg: 3.0, hud_scale: 1.0, opacity: 1.0, lead_ms: 0, flare_tol_deg: 0.3, ladder_step_deg: 6,
           language: '', show_bird: true, show_fpa_num: true, show_vs_num: true, show_flare: true, show_glide: true,
           show_horizon: true, show_hdg: true, show_pitch_minor: true, show_runway: true, show_att: true, show_speed: true,
           show_alt: true, show_vs: true, show_roll: true, show_gs: true, show_bird_ra: true, bird_ra_below_ft: 500, xwind_mode: false,
           ui_style: 'tool', camera_source: 'chaseplane', auto_show: true, auto_show_kt: 40, auto_hide: false, auto_hide_ft: 2500, auto_hide_s: 30,
           // the HUD's look (core/settings.h): layout, element styles, finish, animations, approach marks
           lay_spread: 1, spd_dx: 0, spd_dy: 0, spd_len: 1, spd_size: 1, alt_dx: 0, alt_dy: 0, alt_len: 1, alt_size: 1,
           vs_dx: 0, vs_dy: 0, vs_size: 1, wind_dx: 0, wind_dy: 0, wind_size: 1, roll_dy: 0, roll_size: 1, att_size: 1,
           bird_size: 1, ladder_w: 1, hdg_dy: 0, text_size: 1, line_w: 1,
           sty_fpv: 'airbus', sty_speed: 'airbus', sty_alt: 'airbus', sty_vs: 'needle', sty_ladder: 'airbus', sty_hdg: 'horizon',
           sty_roll: 'airbus', show_accel: false, hud_font: 'bahnschrift', hud_color: '#00ff00', hud_outline: 1, hud_glow: 0,
           anim_in: 'fade', anim_out: 'fade', anim_time: 1, hud_preset: 'airbus',
           show_tdz: true, tdz_rule: 'icao', tdz_style: 'aim', flare_ra_ft: 50, flare_hi_ft: 40, flare_lo_ft: 10, flare_end_deg: -0.5,
           terrain_on: false, terrain_look: 'fill', terrain_strength: 1, terrain_stream: true, terrain_res: 30, terrain_cache_gb: 2,
           terrain_notes: true,
           hotkeys: { toggle: 'Ctrl+Alt+Insert', xwind: 'Ctrl+Alt+Shift+Insert', glide_up: 'Ctrl+Alt+PgUp', glide_down: 'Ctrl+Alt+PgDn',
                      lead_up: 'Ctrl+Alt+Home', lead_down: 'Ctrl+Alt+End', bright_up: '', bright_down: '', size_up: '', size_down: '',
                      preset_next: '', anim_test: '', panel: '', quit: 'Ctrl+Alt+Shift+End' } },
    st: null
  };
  var touched = {};  // key -> time of our own last change (incoming state does not fight a drag)

  // ---------------------------------------------------------------- words
  var T = {
    zh: {
      sym: '符号', symCount: '{n} / {all} 显示中', fine: '细调', path: '飞行路径', world: '天地', inst: '仪表',
      size: '大小', bright: '亮度', glide: '下滑角', lead: '显示延迟补偿', tol: '拉平提示容差', ladder: '俯仰刻度间隔',
      rah: '小鸟下的 RA 从多高开始出现', ra: '无线电高度校准', xw: '侧风模式：仪表带收窄',
      raBtn: '停在地面时，记下现在的读数', raBtnWait: '停稳在地面上才能校准', raClear: '清除', raNone: '这架飞机还没校准；第一次停稳在地面时会自动校准',
      power: '显示 HUD', raNoAc: '连上模拟器后，按机型分别记住', raFmt: '读数减去 {ft} ft', off: '关', auto: '随 Windows 启动', log: '日志', quit: '退出 Birdie',
      quitAsk: '再点一次退出', gIdle: '悬停一个符号，HUD 上那一笔会亮起来', nowOff: '现在关着，点一下打开',
      look: '界面风格', look_tool: '工具', look_light: '浅色', look_dusk: '黄昏',
      autoNav: '自动显示 / 隐藏', as: '滑跑超过这个速度才显示', ah: '高空自动隐藏', ahFt: '无线电高度高于', ahS: '持续',
      autoDesc: '地面上速度不够时 HUD 不显示，起飞滑跑时自动出现。高空隐藏后，无线电高度一降到设定值以下就回来。',
      hudLook: 'HUD 外观', preset: '整套风格', pre_airbus: '空客', pre_boeing: '波音', pre_fighter: '战斗机', pre_minimal: '极简', pre_custom: '自定义',
      layout: '布局', animNav: '开场 / 退场', elements: '各部分的样式', finish: '颜色与线条',
      fpv: '飞行路径符号', spdS: '速度', altS: '高度', vsS: '升降速度', ladderS: '俯仰刻度', hdgS: '航向', rollS: '坡度',
      s_airbus: '空客', s_boeing: '波音', s_fighter: '战斗机', s_minimal: '极简', s_ring: '圆环', s_cross: '十字', s_box: '只要读数框',
      s_needle: '指针', s_digital: '数字', s_horizon: '地平线上', s_arc: '底部罗盘', s_tape: '顶部航向带', s_bottom: '底部',
      accel: '加速度尖括号（波音）', color: '颜色', font: '字体', outline: '描边', glow: '光晕',
      c_green: '绿', c_soft: '柔绿', c_lime: '黄绿', c_white: '白', c_cyan: '青', c_amber: '琥珀', c_magenta: '品红', c_custom: '自定',
      g_all: '整体', g_spd: '速度带', g_alt: '高度带', g_vs: '升降速度', g_wind: '地速与风', g_roll: '坡度刻度', g_misc: '其他',
      l_spread: '左右张开', l_text: '文字大小', l_line: '线宽', l_dx: '左右', l_dy: '上下', l_len: '长度', l_sz: '大小',
      l_bird: '小鸟大小', l_att: '机头符号', l_ladder: '俯仰刻度宽', l_hdg: '航向上下',
      layReset: '恢复默认布局', layDesc: '左右、上下的单位是度（HUD 上的角度）。调的时候 HUD 会一直显示，在地面上也能看。',
      animIn: '开场', animOut: '退场', animTime: '时长', a_none: '无', a_fade: '淡入淡出', a_iris: '光圈', a_boot: '开机',
      a_scan: '扫描', a_assemble: '组装', tryIn: '预览开场', tryOut: '预览退场',
      animDesc: '淡入淡出、光圈偏优雅；开机、扫描、组装偏酷炫。预览要在驾驶舱视角下、HUD 能显示时才看得到。',
      appr: '进近：接地区 · 拉平', tdz: '跑道上标出接地区', tdzRule: '规则', tdzStyle: '显示', t_icao: 'ICAO', t_faa: 'FAA',
      t_aim: '瞄准点和终点', t_full: '全部标志', t_end: '只标终点', flareG: '拉平提示', flareRa: '从多高开始提示',
      flareHi: '开始拉平的高度', flareLo: '拉平结束的高度', flareEnd: '接地时的航迹角',
      apprDesc: '标志的位置按跑道长度取 ICAO 附件 14 或 FAA AC 150/5340-1；"终点"是接地区结束的地方（900 m / 3000 ft，最多跑道的三分之一）。拉平提示：+ 正好，++ 还太陡、继续拉。',
      pv: '预览', pvAway: '预览 · HUD 此刻不在屏幕上', pvOffline: '预览', pvDim: '按 {min}% 画，实际 {v}%',
      border: 'HUD 边框', seatL: '机长位', seatR: '副驾位', bdNone: '还没描，HUD 用的是整块合像镜', bdDrawn: '已经描好',
      bdMirrored: '按另一座镜像', bdShortDrawn: '已描', bdShortMirrored: '镜像', bdShortNone: '未设',
      bdEdit: '在画面上描 ›', bdEditNo: '进驾驶舱视角后才能描', bdEditing: '正在画面上描 · Enter 存好 · Esc 不改',
      bdEye: '眼点和描的时候不一样了，边框可能偏，可以重描', bdCut: '仪表有一部分在边框外，会淡掉',
      bdFit: '缩到 {n}% 放得下', bdNoFit: '缩到最小也放不下', bdMirror: '镜像到另一座', bdClear: '清除', bdClearAsk: '再点一次清除',
      bdDesc: '边框跟着驾驶舱走：改缩放、转头，它都贴着窗框；最后 1° 慢慢淡出。另一座没描时，按这边镜像。',
      camNav: '镜头来源', cam: '来源', cs_chaseplane: 'ChasePlane', cs_msfs: 'MSFS 原生 · Beta', camSt: '现在',
      camOk: ' · 读到驾驶舱镜头', camOutside: ' · 读到镜头，但不是驾驶舱视角', camNone: ' · 读不到镜头', camNoCp: ' · 没读到 ChasePlane',
      camNoSim: ' · 没连上模拟器', camReplay: ' · 回放中（用录下的 ChasePlane 数据）', camFov: '垂直视场角 {v}°', camErr: '最近一次出错：{e}',
      camUserOff: 'MSFS 设置里关掉了插件镜头', camWait: '等模拟器送来镜头数据',
      camOrigin: '镜头原点（{ac}）：已标定，{v} m', camOriginNeed: '镜头原点（{ac}）还没标定：在地面停稳几秒，或在空中按一下暂停，会自己标定；标定前跑道框会偏',
      camOriginNoAc: '镜头原点：等模拟器报出机型', camRedo: '重新标定',
      camDesc: 'ChasePlane：用 ChasePlane 的驾驶舱镜头定位 HUD（默认）。MSFS 原生：直接读模拟器自己的镜头，不需要 ChasePlane。它用的是 MSFS 还在测试的接口（Beta），模拟器更新后可能失效，那时切回 ChasePlane 就行。两种都只读镜头，不会动你的视角。',
      keys: '快捷键', keysN: '{n} 个已设', k_hud: 'HUD', k_tune: '微调', k_toggle: '显示 / 隐藏 HUD', k_xwind: '侧风模式',
      k_preset_next: '换下一套风格', k_anim_test: '播放开场', k_panel: '打开设置窗口', k_quit: '退出 Birdie',
      k_glide_up: '下滑参考线 +0.1°', k_glide_down: '下滑参考线 −0.1°', k_lead_up: '显示延迟补偿 +10 ms', k_lead_down: '显示延迟补偿 −10 ms',
      k_bright_up: '亮度 +5%', k_bright_down: '亮度 −5%', k_size_up: '大小 +5%', k_size_down: '大小 −5%',
      hkNone: '未设', hkListen: '按下组合键…', hkTaken: '被占用', hkNeedMod: '要带 Ctrl / Alt / Shift', hkReset: '恢复默认快捷键',
      hkDesc: '点右边的组合键，再按下新的；Esc 不改，退格清除。快捷键在任何窗口里都有效；和别的程序重了会标“被占用”，换一个就好。',
      ig: '游戏内面板', igNone: '没装', igCurrent: '已装好', igOld: '旧版本', igForeign: '有同名文件夹', igNoFolder: '没找到 Community 文件夹',
      igDescNone: '装上后，模拟器顶部工具栏里会有小鸟，飞行中不用切出去也能调。可选，不装也能用。',
      igDescCurrent: '在 {dir}', igDescOld: '更新没成功：{err}', igDescOldPlain: '和这一版的文件不一样，可以更新',
      igDescForeign: '{dir} 里已经有一个 birdie-panel 文件夹，不是 Birdie 的，Birdie 不会动它。',
      igDescNoFolder: '没在 UserCfg.opt 里找到 MSFS 2024 的 Community 文件夹，可以自己选。',
      igRestart: '装好了。模拟器开着，重启后小鸟才会出现在工具栏。', igRemoved: '已移除。模拟器开着的话，重启后工具栏里就没有了。',
      igExported: '已导出。', igFailed: '没成功：{err}', igInSim: '你正在用的就是它。',
      igInstall: '安装', igUpdate: '更新', igRemove: '移除', igRemoveAsk: '再点一次移除', igOpen: '打开文件夹',
      igPick: '选择 Community 文件夹…', igAuto: '改回自动查找', igExport: '导出到别处…',
      wTitle: '开始之前', wCp: '需要 ChasePlane：HUD 跟着它的驾驶舱镜头摆位置。', wWin: '全屏、无边框、窗口化都可以；VR 下看不到 HUD。',
      wKey: '显示 / 隐藏 HUD：{key}', wTray: '这个窗口随时可以从托盘里的小鸟打开。',
      wIgNone: '要不要顺便装上游戏内面板？装上后，在模拟器的工具栏里也能调这些设置。',
      wIgNoFolder: '没找到 Community 文件夹；以后可以在“细调”里装游戏内面板。',
      wDone: '游戏内面板装好了。重启模拟器后，小鸟出现在顶部工具栏。', wInstall: '装上游戏内面板', wOk: '知道了',
      terrain: '地形', trOn: '在 HUD 上显示地形',
      trIntro: '山坡铺一层淡淡的 HUD 绿，最上面一条天际线。平地不论多高都透明，跑道、城市和高原机场不会被盖住。高程数据来自 Copernicus，需要联网下载，所以默认关着。',
      trLook: '画法', tl_fill: '坡面 + 天际线', tl_sky: '只要天际线', tl_ridges: '坡面 + 天际线 + 前方山脊', trStrength: '浓淡',
      trStream: '飞到哪，自动下载到哪', trRes: '精度（每块 1°×1°）', r_30: '30 m · 约 26 MB/块', r_90: '90 m · 约 3 MB/块',
      trCap: '缓存最多占用', trNotes: '在 HUD 上提示下载进度',
      trsOn: '只在 HUD 打开时下载飞机周围 100 km 的地形，存进缓存；超过上限时，先删最久没用到的。',
      trsBusy: '正在下载 {tile}（{i}/{n}）{rate}', trsHudOff: 'HUD 关着，不下载。', trsNoSim: '连上模拟器、HUD 打开后才开始下载。',
      trsManual: '不自动下载：只用下面装好的离线包，没装的地方没有地形。',
      trDisk: '占用的磁盘', trUseCache: '缓存 {n} 块 · {size}（上限 {cap}）', trUsePkg: '离线包 {n} 块 · {size}',
      trUseFree: '这个盘还剩 {free}', trUseNone: '还没有下载任何地形。',
      trOpen: '打开文件夹', trMove: '换个位置…', trMoveDef: '搬回默认位置', trMoving: '正在搬到新位置… {done}/{total}',
      trClear: '清空缓存', trClearAsk: '再点一次清空缓存', trDelAll: '删除全部离线包', trDelAllAsk: '再点一次全部删除',
      trDiskNote: '这些都是普通文件，放在上面这个文件夹里，随时可以删。卸载 Birdie 不会自动删掉它们，不用了请先在这里清空。',
      trPkg: '离线包', trPkgDesc: '把常飞的地方提前下好：飞行时不用等，也不会被缓存清理掉。在下面选地区。',
      trGet: '下载', trDel: '删除', trDelAsk: '再点一次删除', trHave: '已装好', trTiles: '{have}/{n} 块 · {size}',
      trJob: '正在下载 {label}：{done}/{total} {rate}', trStop: '停止', trJobFail: '，{n} 块失败',
      trImpDesc: '也可以自己下载：Copernicus 的 .tif（或整包 .zip / .tar）放进 import 文件夹，再点“导入”。导入成功的原文件会删掉。',
      trImpHas: '现在里面有 {n} 个文件。', trOpenImp: '打开 import 文件夹', trImport: '导入', trImporting: '正在导入… {done}/{total}',
      e_network: '连不上下载服务器（网络？），过一会儿自动再试', e_disk_full: '磁盘空间不够了（要留至少 1 GB）', e_other: '有一块没成功：{e}',
      trInSim: '下载位置、导入在 Birdie 的设置窗口里管理（托盘里的小鸟）。',
      trBorder: '地形边框', tbNone: '· 跟着 HUD 边框', tbDrawn: '· 单独描好了', tbMirrored: '· 按另一座镜像',
      tbNote: '地形可以有自己的范围：比如铺满整块风挡，HUD 符号仍留在合像镜里；也可以比 HUD 小。按机型和座位分别记住，没描时跟着 HUD 边框。',
      tbOff: '打开地形后才能描。',
      trCredit: '高程数据：produced using Copernicus WorldDEM-30 / -90 © DLR e.V. 2010–2014 and © Airbus Defence and Space GmbH 2014–2018 provided under COPERNICUS by the European Union and ESA; all rights reserved。Copernicus 计划的主管机构对这些数据的任何使用均不承担责任，也没有认可 Birdie。只用于模拟飞行，不能用于真实导航。'
    },
    en: {
      sym: 'Symbols', symCount: '{n} / {all} shown', fine: 'Fine tuning', path: 'Flight path', world: 'Sky & ground', inst: 'Instruments',
      size: 'Size', bright: 'Bright', glide: 'Glide', lead: 'Display latency', tol: 'Flare cue tolerance', ladder: 'Pitch scale every',
      rah: 'Radio altitude under the bird from', xw: 'Crosswind mode: narrow tapes',
      ra: 'Radio altitude zero', raBtn: 'Take the reading now (on the ground)', raBtnWait: 'Stand still on the ground to calibrate',
      raClear: 'clear', raNone: 'not set for this aircraft yet; it sets itself the first time you stand still on the ground',
      raNoAc: 'kept per aircraft once the sim is connected', raFmt: 'reading minus {ft} ft', off: 'off', auto: 'Start with Windows',
      power: 'Show HUD', log: 'Log', quit: 'Quit Birdie', quitAsk: 'Click again to quit', gIdle: 'Hover a symbol and its stroke lights up on the HUD',
      look: 'Look', look_tool: 'Tool', look_light: 'Light', look_dusk: 'Dusk',
      autoNav: 'Show / hide by itself', as: 'Show once the roll passes', ah: 'Hide when high', ahFt: 'Radio altitude above', ahS: 'for',
      autoDesc: 'On the ground the HUD waits until the take-off roll passes the set speed. Hidden up high, it comes back as soon as the radio altitude drops below the set height.',
      hudLook: 'HUD look', preset: 'Preset', pre_airbus: 'Airbus', pre_boeing: 'Boeing', pre_fighter: 'Fighter', pre_minimal: 'Minimal', pre_custom: 'Custom',
      layout: 'Layout', animNav: 'Opening / closing', elements: 'Symbols', finish: 'Colour & line',
      fpv: 'Flight path symbol', spdS: 'Speed', altS: 'Altitude', vsS: 'Vertical speed', ladderS: 'Pitch scale', hdgS: 'Heading', rollS: 'Bank',
      s_airbus: 'Airbus', s_boeing: 'Boeing', s_fighter: 'Fighter', s_minimal: 'Minimal', s_ring: 'Ring', s_cross: 'Cross', s_box: 'Box only',
      s_needle: 'Needle', s_digital: 'Number', s_horizon: 'On the horizon', s_arc: 'Compass arc', s_tape: 'Top tape', s_bottom: 'Bottom',
      accel: 'Acceleration caret (Boeing)', color: 'Colour', font: 'Font', outline: 'Outline', glow: 'Glow',
      c_green: 'Green', c_soft: 'Soft green', c_lime: 'Lime', c_white: 'White', c_cyan: 'Cyan', c_amber: 'Amber', c_magenta: 'Magenta', c_custom: 'Own',
      g_all: 'Overall', g_spd: 'Speed tape', g_alt: 'Altitude tape', g_vs: 'Vertical speed', g_wind: 'Ground speed & wind', g_roll: 'Bank scale', g_misc: 'Other',
      l_spread: 'Spread', l_text: 'Text', l_line: 'Lines', l_dx: 'Across', l_dy: 'Up/down', l_len: 'Length', l_sz: 'Size',
      l_bird: 'Bird', l_att: 'Nose mark', l_ladder: 'Rungs', l_hdg: 'Heading',
      layReset: 'Reset the layout', layDesc: 'Offsets are in degrees on the HUD. While you adjust, the HUD stays on, even on the ground.',
      animIn: 'Opening', animOut: 'Closing', animTime: 'Duration', a_none: 'None', a_fade: 'Fade', a_iris: 'Iris', a_boot: 'Power-on',
      a_scan: 'Scan', a_assemble: 'Assemble', tryIn: 'Play the opening', tryOut: 'Play the closing',
      animDesc: 'Fade and iris are quiet; power-on, scan and assemble are showy. Previews play on the HUD, from the cockpit view.',
      appr: 'Approach: touchdown zone · flare', tdz: 'Touchdown zone on the runway', tdzRule: 'Rule', tdzStyle: 'Show', t_icao: 'ICAO', t_faa: 'FAA',
      t_aim: 'Aiming point + end', t_full: 'All marks', t_end: 'End only', flareG: 'Flare cue', flareRa: 'Cue from',
      flareHi: 'Flare starts at', flareLo: 'Flare ends at', flareEnd: 'Path angle at touchdown',
      apprDesc: 'Marks placed by runway length after ICAO Annex 14 or FAA AC 150/5340-1; the end is where the touchdown zone ends (900 m / 3000 ft, at most a third of the runway). Flare cue: + on profile, ++ still steep, keep pulling.',
      nowOff: 'now off, tap to show', pv: 'Preview', pvAway: 'Preview · the HUD is not on the screen now', pvOffline: 'Preview',
      pvDim: 'drawn at {min}%, set to {v}%',
      border: 'HUD border', seatL: 'left seat', seatR: 'right seat', bdNone: 'Not traced: the HUD uses its whole combiner', bdDrawn: 'Traced',
      bdMirrored: 'Mirrored from the other seat', bdShortDrawn: 'traced', bdShortMirrored: 'mirrored', bdShortNone: 'none',
      bdEdit: 'Trace it on the screen ›', bdEditNo: 'Trace it from the cockpit view', bdEditing: 'Tracing on the screen · Enter saves · Esc cancels',
      bdEye: 'The eye point moved since tracing; the border may be off — trace again', bdCut: 'Part of the instruments is outside the border and fades',
      bdFit: 'shrink to {n}% to fit', bdNoFit: 'does not fit even at the smallest size', bdMirror: 'mirror to the other seat', bdClear: 'clear',
      bdClearAsk: 'once more to clear',
      bdDesc: 'The border stays with the cockpit through zoom and head turns, fading over its last degree. The other seat mirrors it until traced.',
      camNav: 'Camera source', cam: 'Source', cs_chaseplane: 'ChasePlane', cs_msfs: 'MSFS native · Beta', camSt: 'Now',
      camOk: ' · cockpit camera read', camOutside: ' · camera read, not a cockpit view', camNone: ' · no camera data', camNoCp: ' · ChasePlane not read',
      camNoSim: ' · no simulator', camReplay: ' · replay (recorded ChasePlane data)', camFov: 'Vertical field of view {v}°', camErr: 'Last error: {e}',
      camUserOff: 'Add-on cameras are switched off in the MSFS settings', camWait: 'Waiting for camera data from the sim',
      camOrigin: 'Camera origin ({ac}): measured, {v} m', camOriginNeed: 'Camera origin ({ac}) not measured yet: stand still on the ground a few seconds, or pause in the air, and it measures itself; until then the runway outline is off',
      camOriginNoAc: 'Camera origin: waiting for the sim to name the aircraft', camRedo: 'Measure again',
      camDesc: 'ChasePlane: the HUD follows the ChasePlane cockpit camera (the default). MSFS native: reads the sim’s own camera, no ChasePlane needed. It uses an interface MSFS still calls beta: an update may break it, and then switching back to ChasePlane is all it takes. Either way Birdie only reads the camera and never moves your view.',
      keys: 'Hotkeys', keysN: '{n} set', k_hud: 'HUD', k_tune: 'Fine tuning', k_toggle: 'Show / hide the HUD', k_xwind: 'Crosswind mode',
      k_preset_next: 'Next preset', k_anim_test: 'Play the opening', k_panel: 'Open the settings window', k_quit: 'Quit Birdie',
      k_glide_up: 'Glide reference +0.1°', k_glide_down: 'Glide reference −0.1°', k_lead_up: 'Display latency +10 ms', k_lead_down: 'Display latency −10 ms',
      k_bright_up: 'Brightness +5%', k_bright_down: 'Brightness −5%', k_size_up: 'Size +5%', k_size_down: 'Size −5%',
      hkNone: 'none', hkListen: 'Press the keys…', hkTaken: 'taken', hkNeedMod: 'Needs Ctrl, Alt or Shift', hkReset: 'Reset the hotkeys',
      hkDesc: 'Click a combo, then press the new one; Esc keeps it, Backspace clears it. Hotkeys work from any window; one another program already uses is marked taken.',
      ig: 'In-sim panel', igNone: 'not installed', igCurrent: 'installed', igOld: 'older version', igForeign: 'name taken', igNoFolder: 'no Community folder',
      igDescNone: 'Puts the bird in the sim’s toolbar, to adjust things without leaving the flight. Optional: Birdie works without it.',
      igDescCurrent: 'in {dir}', igDescOld: 'Update failed: {err}', igDescOldPlain: 'Its files differ from this version; update it',
      igDescForeign: 'There is a birdie-panel folder in {dir} that is not Birdie’s; Birdie leaves it alone.',
      igDescNoFolder: 'The MSFS 2024 Community folder was not found in UserCfg.opt. Choose it yourself.',
      igRestart: 'Installed. The sim is running: the bird shows in the toolbar after a restart.',
      igRemoved: 'Removed. With the sim running, the toolbar changes after a restart.',
      igExported: 'Exported.', igFailed: 'Did not work: {err}', igInSim: 'You are using it right now.',
      igInstall: 'Install', igUpdate: 'Update', igRemove: 'Remove', igRemoveAsk: 'Once more to remove', igOpen: 'Open folder',
      igPick: 'Choose the Community folder…', igAuto: 'Find it by itself', igExport: 'Export elsewhere…',
      wTitle: 'Before you fly', wCp: 'Needs ChasePlane: the HUD follows its cockpit camera.',
      wWin: 'Full screen, borderless or windowed all work; the HUD does not show in VR.',
      wKey: 'Show / hide the HUD: {key}', wTray: 'Open this window any time from the bird in the tray.',
      wIgNone: 'Install the in-sim panel too? Then the same settings sit in the sim’s toolbar.',
      wIgNoFolder: 'No Community folder found; you can install the in-sim panel later under Fine tuning.',
      wDone: 'In-sim panel installed. Restart the sim and the bird is in the top toolbar.', wInstall: 'Install the in-sim panel', wOk: 'Got it',
      terrain: 'Terrain', trOn: 'Show terrain on the HUD',
      trIntro: 'A faint HUD-green shade on the slopes and a skyline on top. Flat ground stays clear at any height, so runways, cities and high airports are never covered. The heights come from Copernicus and are downloaded, so it starts switched off.',
      trLook: 'Drawing', tl_fill: 'Slopes + skyline', tl_sky: 'Skyline only', tl_ridges: 'Slopes + skyline + ridges', trStrength: 'Strength',
      trStream: 'Download where I fly, by itself', trRes: 'Detail (per 1°×1° tile)', r_30: '30 m · about 26 MB a tile', r_90: '90 m · about 3 MB a tile',
      trCap: 'Cache uses at most', trNotes: 'Show download progress on the HUD',
      trsOn: 'Only while the HUD is on, the terrain 100 km around the aircraft is downloaded into the cache; above the limit, the tiles used longest ago go first.',
      trsBusy: 'Downloading {tile} ({i}/{n}) {rate}', trsHudOff: 'The HUD is off: nothing is downloaded.', trsNoSim: 'Downloads start once the sim is connected and the HUD is on.',
      trsManual: 'No downloads by itself: only the packages below are used; elsewhere there is no terrain.',
      trDisk: 'Disk use', trUseCache: 'Cache {n} tiles · {size} (limit {cap})', trUsePkg: 'Packages {n} tiles · {size}',
      trUseFree: '{free} free on this drive', trUseNone: 'No terrain downloaded yet.',
      trOpen: 'Open folder', trMove: 'Move elsewhere…', trMoveDef: 'Move back to the default', trMoving: 'Moving to the new place… {done}/{total}',
      trClear: 'Clear the cache', trClearAsk: 'Once more to clear the cache', trDelAll: 'Delete all packages', trDelAllAsk: 'Once more to delete them all',
      trDiskNote: 'These are plain files in the folder above and can be deleted any time. Uninstalling Birdie does not remove them: clear them here first if you stop using terrain.',
      trPkg: 'Packages', trPkgDesc: 'Download the places you fly ahead of time: no waiting in flight, and the cache never trims them. Pick a region below.',
      trGet: 'Download', trDel: 'Delete', trDelAsk: 'Once more to delete', trHave: 'installed', trTiles: '{have}/{n} tiles · {size}',
      trJob: 'Downloading {label}: {done}/{total} {rate}', trStop: 'Stop', trJobFail: ', {n} failed',
      trImpDesc: 'Or download them yourself: put Copernicus .tif files (or the .zip / .tar they come in) into the import folder and press Import. Imported files are deleted.',
      trImpHas: '{n} files in it now.', trOpenImp: 'Open the import folder', trImport: 'Import', trImporting: 'Importing… {done}/{total}',
      e_network: 'Cannot reach the download server (network?); trying again shortly', e_disk_full: 'Not enough disk space (at least 1 GB stays free)', e_other: 'A tile did not work: {e}',
      trInSim: 'Where the data goes, and importing, live in Birdie’s settings window (the bird in the tray).',
      trBorder: 'Terrain border', tbNone: '· follows the HUD border', tbDrawn: '· traced', tbMirrored: '· mirrored from the other seat',
      tbNote: 'The terrain can have a reach of its own: the whole windscreen, say, while the HUD symbols stay in the combiner, or smaller than the HUD. Kept per aircraft and seat; until traced it follows the HUD border.',
      tbOff: 'Switch the terrain on to trace it.',
      trCredit: 'Heights: produced using Copernicus WorldDEM-30 / -90 © DLR e.V. 2010–2014 and © Airbus Defence and Space GmbH 2014–2018 provided under COPERNICUS by the European Union and ESA; all rights reserved. The organisations in charge of the Copernicus programme by law or by delegation do not incur any liability for any use of the Copernicus WorldDEM-30, and do not endorse Birdie. For flight simulation only, not for real-world navigation.'
    }
  };
  // the dusk look says some words its own way
  var T_DUSK = {
    zh: { auto: '开机时陪着你', quit: '让 Birdie 休息', quitAsk: '再点一次，它就去睡了' },
    en: { auto: 'With Windows', quit: 'Let Birdie rest', quitAsk: 'Once more, and it goes to sleep' }
  };
  // the status line: [line, fact] per state from Birdie (status.why). The tools say what is going on; dusk keeps the
  // companion's voice
  var VOICE_TOOL = {
    show:     { zh: ['HUD 显示中', '{rw} · 下滑 {g}°'], en: ['HUD on', '{rw} · glide {g}°'] },
    look:     { zh: ['HUD 暂时隐藏', '视线偏开了，转回前方就出现'], en: ['HUD hidden', 'Looking aside; look ahead to show'] },
    ext:      { zh: ['HUD 不显示', '外部视角'], en: ['HUD not shown', 'Outside view'] },
    taxi:     { zh: ['HUD 待命', '起飞滑跑超过 {kt} kt 时显示'], en: ['HUD standing by', 'Shows above {kt} kt on the take-off roll'] },
    high:     { zh: ['HUD 自动隐藏', '无线电高度低于 {ft} ft 时显示'], en: ['HUD hidden up high', 'Shows below {ft} ft radio altitude'] },
    away:     { zh: ['HUD 暂停', 'MSFS 不在前台'], en: ['HUD paused', 'MSFS is not in front'] },
    pause:    { zh: ['HUD 暂停', '模拟器暂停了，或者数据停了'], en: ['HUD paused', 'The sim is paused, or the data stopped'] },
    nosim:    { zh: ['没连上模拟器', '打开 MSFS 后自动连上'], en: ['No simulator', 'Connects by itself when MSFS starts'] },
    wait:     { zh: ['等待数据', '等模拟器送来第一帧数据'], en: ['Waiting for data', 'Waiting for the first data from the sim'] },
    starting: { zh: ['启动中', '正在找模拟器'], en: ['Starting', 'Looking for the simulator'] },
    nocp:     { zh: ['需要 ChasePlane', '没读到 ChasePlane；也可以在 细调 → 镜头来源 改用 MSFS 原生镜头'], en: ['ChasePlane needed', 'ChasePlane not read; or use the MSFS camera in Fine tuning → Camera source'] },
    nocam:    { zh: ['读不到镜头', 'MSFS 原生镜头没有数据；可以在 细调 → 镜头来源 切回 ChasePlane'], en: ['No camera data', 'The MSFS camera sends nothing; switch back to ChasePlane in Fine tuning → Camera source'] },
    off:      { zh: ['HUD 已关闭', '点右边的开关，或按 {hk} 打开', '点右边的开关打开'], en: ['HUD off', 'Use the switch, or {hk}, to show it', 'Use the switch to show it'] },
    offline:  { zh: ['Birdie 没在运行', '打开 Birdie.exe，这里会自己连上'], en: ['Birdie is not running', 'Start Birdie.exe and this connects by itself'] }
  };
  var VOICE_DUSK = {
    show:     { zh: ['在你身边', '{rw} · 下滑 {g}°'], en: ['Right here with you', '{rw} · glide {g}°'] },
    look:     { zh: ['转回前方，我就在', '视线偏开时先藏起来'], en: ['Look ahead, I’m there', 'Hidden while you look aside'] },
    ext:      { zh: ['回到驾驶舱，我们一起看', '外部视角下不显示'], en: ['Back in the cockpit, together', 'Not shown in outside views'] },
    taxi:     { zh: ['滑行时，先休息一下', '起飞滑跑超过 {kt} kt 时出现'], en: ['Resting while you taxi', 'Appears above {kt} kt on the roll'] },
    high:     { zh: ['高空里，我先歇一会儿', '降到 {ft} ft 以下就回来'], en: ['Resting up high', 'Back below {ft} ft'] },
    away:     { zh: ['你回来，我就回来', 'MSFS 不在前台'], en: ['Come back and so will I', 'MSFS is not in front'] },
    pause:    { zh: ['等你', '模拟器暂停了，或者数据停了'], en: ['Waiting for you', 'The sim is paused, or the data stopped'] },
    nosim:    { zh: ['等你起飞', '打开 MSFS 后自动连上'], en: ['Ready when you fly', 'Connects by itself when MSFS starts'] },
    wait:     { zh: ['我在听', '等模拟器送来第一帧数据'], en: ['Listening', 'Waiting for the first data from the sim'] },
    starting: { zh: ['我醒了', '正在找模拟器'], en: ['Awake', 'Looking for the simulator'] },
    nocp:     { zh: ['还看不到你的视线', '没读到 ChasePlane；也可以改用 MSFS 原生镜头'], en: ['I can’t see your view yet', 'ChasePlane not read; or try the MSFS camera'] },
    nocam:    { zh: ['还看不到你的视线', '读不到 MSFS 镜头，可以切回 ChasePlane'], en: ['I can’t see your view yet', 'No MSFS camera data; ChasePlane is one switch away'] },
    off:      { zh: ['我先安静一会儿', '点右边的圆，或按 {hk} 叫我回来', '点右边的圆叫我回来'], en: ['I’ll stay quiet for now', 'Tap the circle, or {hk}, to bring me back', 'Tap the circle to bring me back'] },
    offline:  { zh: ['我还没醒', '打开 Birdie.exe，这里会自己连上'], en: ['Not awake yet', 'Start Birdie.exe and this connects by itself'] }
  };
  // [key, setting, zh, en, zh description, en description, icon]
  var GLYPHS = {
    path: [
      ['bird', 'show_bird', '小鸟', 'Bird', '飞机真正飞去的方向', 'Where the aircraft is really going',
        '<circle class="ic" cx="17" cy="19" r="4.5"/><path class="ic" d="M12.5 19H3M21.5 19H31M17 14.5V8"/>'],
      ['fpa', 'show_fpa_num', '航迹角', 'Path angle', '小鸟右边：现在的航迹角', 'Right of the bird: flight path angle',
        '<circle class="ic" cx="9" cy="19" r="3.5"/><path class="ic" d="M12.5 19h4"/><text class="t" x="18" y="22" font-size="11">3.0</text>'],
      ['vsn', 'show_vs_num', '升降速度', 'Vert. speed', '小鸟左边：升降速度（ft/min）', 'Left of the bird: vertical speed (ft/min)',
        '<text class="t" x="1" y="22" font-size="11">700</text><path class="ic" d="M19 19h3.5"/><circle class="ic" cx="26" cy="19" r="3.5"/>'],
      ['bra', 'show_bird_ra', 'RA 高度', 'Radio alt', '小鸟下方：无线电高度，低于细调里设的高度才出现', 'Under the bird: radio altitude, below the height set in Fine tuning',
        '<circle class="ic" cx="17" cy="10" r="3.5"/><path class="ic" d="M13.5 10H7M20.5 10H27"/><text class="t" x="17" y="27" font-size="11" text-anchor="middle">320</text>'],
      ['flare', 'show_flare', '拉平提示', 'Flare cue', '50 ft 以下在小鸟上方闪：+ 正好，++ 还太陡、继续拉', 'Below 50 ft above the bird: + on profile, ++ still steep, keep pulling',
        '<path class="ic" d="M5 17h9M9.5 12.5v9M20 17h9M24.5 12.5v9"/>'],
      ['glide', 'show_glide', '下滑线', 'Glide line', '理想下滑角在地面上的那条线', 'The line your ideal glide path makes on the ground',
        '<path class="ic" d="M3 13h28" opacity=".5"/><path class="ic" d="M3 21h6M12 21h6M21 21h6"/>']],
    world: [
      ['horizon', 'show_horizon', '地平线', 'Horizon', '真实的地平线', 'The true horizon',
        '<path class="ic" d="M2 18h30"/><path class="ic" d="M11 18a6 6 0 0 1 12 0" opacity=".55"/>'],
      ['hdg', 'show_hdg', '航向', 'Heading', '地平线上的航向刻度', 'Heading marks on the horizon',
        '<path class="ic" d="M2 13h30M6 13v5M14 13v8M22 13v5M30 13v8"/>'],
      ['minor', 'show_pitch_minor', '俯仰刻度', 'Pitch marks', '每 2.5° 的短刻度', 'Short marks every 2.5°',
        '<path class="ic" d="M12 9h10M12 17h10M12 25h10"/>'],
      ['runway', 'show_runway', '跑道', 'Runway', '选中的跑道轮廓', 'Outline of the runway you are lined up with',
        '<path class="ic" d="M14 7h6l7 21H7z"/>']],
    inst: [
      ['att', 'show_att', '机头', 'Aircraft', '机头指向：读俯仰和坡度', 'Where the nose points',
        '<path class="ic" d="M3 17h9v4M31 17h-9v4"/><path class="ic" d="M15 15h4v4h-4z"/>'],
      ['speed', 'show_speed', '速度', 'Speed', '左侧速度带', 'Speed tape, left',
        '<path class="ic" d="M20 4v26M14 9h6M14 15h6M14 21h6M14 27h6"/><path class="ic" d="M23 17l5-2.5v5z"/>'],
      ['alt', 'show_alt', '高度', 'Altitude', '右侧高度带与无线电高度', 'Altitude tape and radio altitude, right',
        '<path class="ic" d="M12 4v26M12 9h5M12 15h5M12 21h5M12 27h5"/><path class="ic" d="M17 13h12v8H17z"/>'],
      ['vs', 'show_vs', '升降', 'VS needle', '高度带右边的升降速度指针', 'Vertical speed needle',
        '<path class="ic" d="M30 13L8 24"/><path class="ic" d="M6 10v18" opacity=".5"/>'],
      ['roll', 'show_roll', '坡度', 'Bank', '顶上的坡度刻度', 'Bank scale at the top',
        '<path class="ic" d="M4 24a13 13 0 0 1 26 0"/><path class="ic" d="M17 11l-3 5h6z"/>'],
      ['gs', 'show_gs', '地速与风', 'GS & wind', '地速，风向风速', 'Ground speed and wind',
        '<text class="t" x="2" y="15" font-size="11">GS</text><path class="ic" d="M22 28V12M18 16l4-4 4 4"/>']]
  };
  var ALL = [].concat(GLYPHS.path, GLYPHS.world, GLYPHS.inst);
  function glyph(k) { for (var i = 0; i < ALL.length; i++) if (ALL[i][0] === k) return ALL[i]; return null; }

  function $(id) { return document.getElementById(id); }
  var STYLES = ['tool', 'light', 'dusk'];
  function look() { return STYLES.indexOf(S.set.ui_style) >= 0 ? S.set.ui_style : 'tool'; }
  function L() {
    var base = T[S.lang];
    if (look() !== 'dusk') return base;
    var o = {}, k, d = T_DUSK[S.lang];
    for (k in base) o[k] = base[k];
    for (k in d) o[k] = d[k];
    return o;
  }
  function VOICES() { return look() === 'dusk' ? VOICE_DUSK : VOICE_TOOL; }
  // the look is a class on <html>; remembered by the page too, so the next open starts in it before Birdie answers
  function applyLook() {
    var k = look();
    each(STYLES, function (x) { cls(document.documentElement, 'ui-' + x, x === k); });
    try { window.localStorage && localStorage.setItem('birdie.look', k); } catch (e) { /* no storage */ }
  }
  function fmt(s, o) { return s.replace(/\{(\w+)\}/g, function (_, k) { return o[k]; }); }
  function f1(v) { return v.toFixed(1); }
  function each(list, fn) { for (var i = 0; i < list.length; i++) fn(list[i], i); }

  // ---------------------------------------------------------------- where the page runs
  var COUI = !!(window.coherent || /Coherent/i.test(navigator.userAgent));
  var HOST = !COUI && !!(window.chrome && window.chrome.webview);  // Birdie's own window (WebView2)
  var PEEK = HOST && /[?&]peek\b/.test(location.search);  // the strip that floats up over the tray icon
  var WELCOME = !COUI && !PEEK && /[?&]welcome\b/.test(location.search);  // first run (app.cpp opens ?welcome)
  function host(m) { if (HOST) window.chrome.webview.postMessage(JSON.stringify(m)); }

  // ---------------------------------------------------------------- the link to Birdie.exe
  var ws = null, portIx = 0, retry = null;
  function ports() {
    var p = parseInt(location.port, 10);  // served by Birdie itself: that port first
    return (location.protocol === 'http:' && p && PORTS.indexOf(p) >= 0) ? [p].concat(PORTS.filter(function (x) { return x !== p; })) : PORTS;
  }
  function connect() {
    clearTimeout(retry);
    var list = ports(), port = list[portIx % list.length], opened = false;
    try { ws = new WebSocket('ws://127.0.0.1:' + port + '/'); } catch (e) { return later(); }
    ws.onopen = function () { opened = true; send({ type: 'action', name: 'hello' }); };
    ws.onmessage = function (ev) {
      var m; try { m = JSON.parse(ev.data); } catch (e) { return; }
      if (m.type === 'state') onState(m);
      else if (m.type === 'notice') notice(m.text);
    };
    ws.onclose = function () {
      ws = null;
      if (S.online) { S.online = false; paint(); }
      if (!opened) portIx++;  // nobody there: try the next port
      later();
    };
    ws.onerror = function () { try { ws && ws.close(); } catch (e) { /* closed */ } };
  }
  function later() { clearTimeout(retry); retry = setTimeout(connect, portIx % ports().length ? 300 : 2000); }
  function send(m) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(m)); }

  function set(key, value) {
    S.set[key] = value;
    touched[key] = Date.now();
    send({ type: 'set', key: key, value: value });
  }
  function act(name, value) { var m = { type: 'action', name: name }; if (value !== undefined) m.value = value; send(m); }

  function onState(m) {
    var now = Date.now();
    for (var k in m.settings) if (!(touched[k] && now - touched[k] < 700)) S.set[k] = m.settings[k];
    var wasLang = S.lang, wasLook = document.documentElement.className.indexOf('ui-' + look()) >= 0 ? look() : '';
    S.st = m.status;
    S.lang = (m.status && m.status.lang === 'en') ? 'en' : 'zh';
    var first = !S.online;
    S.online = true;
    if (wasLook !== look()) applyLook();
    if (first || wasLang !== S.lang || wasLook !== look()) words(); else paint();
  }

  // ---------------------------------------------------------------- HUD drawing (canvas coordinates, as the program)
  function P(d, cls, extra) { return '<path class="' + (cls || 'stroke') + '" d="' + d + '" ' + (extra || '') + '/>'; }
  function TX(x, y, s, size, anchor) {
    return '<text class="htxt" x="' + x + '" y="' + y + '" font-size="' + size + '" text-anchor="' + (anchor || 'middle') + '">' + s + '</text>';
  }
  // The preview's runway is a real one (45 x 3000 m) seen from a 3° approach at 150 ft, aimed at the middle of the
  // aiming point stripes, so the bird (3° below the horizon) sits on the aiming point as it does in flight. Marks
  // as core/runway.cpp touchdown_marks() for this runway. Angles map linearly to the canvas (D px per degree).
  var RW = { w: 45, len: 3000, h: 150 * 0.3048, fpa: 3.0 }, DEG = 180 / Math.PI;
  function rwMarks(faa) {  // {aim: [[a0, a1, r0, r1]...], bars: [...], end: m}, metres from the threshold / centreline
    var m = { aim: [], bars: [], end: 0 }, ft = 0.3048, hw = RW.w / 2, L = RW.len;
    m.end = Math.min(faa ? 3000 * ft : 900, L / 3);
    function pair(a0, len, gap, w, out) {
      gap = Math.min(gap, 2 * hw - 2 * w - 2);
      out.push([a0, a0 + len, -0.5 * gap - w, -0.5 * gap], [a0, a0 + len, 0.5 * gap, 0.5 * gap + w]);
    }
    var k, b, a, inner;
    if (faa) {
      var g = Math.min(72 * ft, 0.48 * RW.w), groups = [3, 0, 2, 2, 1, 1];
      pair(1020 * ft, 150 * ft, g, 30 * ft, m.aim);
      for (k = 0; k < 6; k++) {
        a = (k + 1) * 500 * ft;
        if (!groups[k] || a + 75 * ft > m.end + 1) continue;
        for (b = 0; b < groups[k]; b++) {
          inner = 0.5 * g + b * 11 * ft;
          if (inner + 6 * ft > hw - 1) break;
          m.bars.push([a, a + 75 * ft, -inner - 6 * ft, -inner], [a, a + 75 * ft, inner, inner + 6 * ft]);
        }
      }
      return m;
    }
    pair(400, 50, 20, 8, m.aim);  // LDA >= 2400 m
    var stripes = [3, 3, 2, 2, 1, 1], placed = 0;
    for (k = 1; placed < 6 && k <= 8; k++) {
      a = 150 * k;
      if (Math.abs(a - 400) < 100 || a + 22.5 > L / 2) continue;
      for (b = 0; b < stripes[Math.min(placed, 5)]; b++) {
        inner = 10 + b * 4.5;
        if (inner + 3 > hw - 1) break;
        m.bars.push([a, a + 22.5, -inner - 3, -inner], [a, a + 22.5, inner, inner + 3]);
      }
      placed++;
    }
    return m;
  }
  var rwAim = 425, rwDist = 0;  // where the bird aims (m past the threshold), and the eye's distance to it
  function rwSetup(faa) {
    var aim = rwMarks(faa).aim[0];
    rwAim = (aim[0] + aim[1]) / 2;
    rwDist = RW.h / Math.tan(RW.fpa / DEG);
  }
  function rwAt(along, across) {  // runway metres -> canvas [x, y]
    var d = rwDist - rwAim + along;
    return [CX + Math.atan2(across, d) * DEG * D, HY + Math.atan2(RW.h, d) * DEG * D];
  }
  function rwTop() { return rwAt(RW.len, 0)[1]; }
  function rwBottom() { return rwAt(0, 0)[1]; }
  function runwayHalf(y) {  // half width in px at canvas y (on the runway surface)
    var dep = Math.max(1e-3, (y - HY) / D), d = RW.h / Math.tan(dep / DEG);
    return Math.atan2(RW.w / 2, d) * DEG * D;
  }
  function rwQuad(q) {
    var p = [rwAt(q[0], q[2]), rwAt(q[0], q[3]), rwAt(q[1], q[3]), rwAt(q[1], q[2])];
    return 'M' + p.map(function (c) { return f1(c[0]) + ' ' + f1(c[1]); }).join('L') + 'Z';
  }
  rwSetup(false);

  // ---- element styles as hud/scene.cpp draws them, in the preview's moment: heading 071, 140 kt, 340 ft,
  // -700 fpm, wings level. Conformal angles map linearly (D px per degree) around (CX, HY).
  var PV = { hdg: 71, ias: 140, alt: 340 };
  function pts(a, close) { return 'M' + a.map(function (q) { return f1(q[0]) + ' ' + f1(q[1]); }).join('L') + (close ? 'Z' : ''); }
  function range(a, b, n) { var o = []; for (var i = 0; i < n; i++) o.push(a + (b - a) * i / (n - 1)); return o; }
  function readoutBox(x0, x1, yc, h, side, tip) {  // scene.cpp readout_box()
    if (side > 0) return pts([[x0, yc - h], [x1, yc - h], [x1 + tip, yc], [x1, yc + h], [x0, yc + h]], true);
    if (side < 0) return pts([[x0, yc - h], [x1, yc - h], [x1, yc + h], [x0, yc + h], [x0 - tip, yc]], true);
    return pts([[x0, yc - h], [x1, yc - h], [x1, yc + h], [x0, yc + h]], true);
  }
  function tapeSpan(len) {  // scene.cpp tape_span(): [top, bottom]
    var xw = !!S.set.xwind_mode;
    if (len == null) len = 1;
    if (!xw && len === 1) return [328.865, 692.13];
    var k = len * (xw ? 0.55 : 1);
    return [510.5 - (510.5 - 328.865) * k, 510.5 + (692.13 - 510.5) * k];
  }
  function TXr(x, y, s, size, ang) {  // text turned by ang degrees about its anchor
    return '<g transform="rotate(' + f1(ang) + ' ' + f1(x) + ' ' + f1(y) + ')">' + TX(f1(x), f1(y), s, size) + '</g>';
  }
  function ladderSvg(s) {  // conformal() rungs: airbus / boeing / fighter (bent) / minimal
    var step = s.ladder_step_deg;
    if (!(step > 0)) return '';
    var ls = s.sty_ladder, boeing = ls === 'boeing', fighter = ls === 'fighter', minimal = ls === 'minimal';
    var inner = (CX - 518.26) / D, outer = (CX - 447.16) / D;
    if (boeing) { inner = 2.6; outer = 6.4; } else if (fighter) { inner = 2.2; outer = 6.0; } else if (minimal) { inner = 1.6; outer = 3.0; }
    var lw = s.ladder_w == null ? 1 : s.ladder_w;
    if (lw !== 1) outer = inner + (outer - inner) * lw;
    var tick = (fighter ? 16 : 11) / D, dashes = boeing ? 3 : minimal ? 2 : 4, dl = (outer - inner) / (dashes * 1.64 - 0.64);
    var kmax = Math.floor(Math.min(s.ladder_max_deg || 30, 30) / step + 1e-6), d = '', lab = '', k, e, side, j, a0, a1, p;
    function at(a, el) { return [CX + a * D, HY - el * D]; }
    function el(a) { return fighter ? e - (e > 0 ? 1 : -1) * (a - inner) * Math.tan(Math.abs(e) / 2 / DEG) : e; }
    for (k = -kmax; k <= kmax; k++) {
      if (!k) continue;
      e = k * step;
      if (HY - e * D < 70 || HY - e * D > 870) continue;
      for (side = -1; side <= 1; side += 2) {
        if (e > 0) {  // solid, end tick down towards the horizon
          p = range(inner, outer, 6).map(function (a) { return at(side * a, el(a)); });
          if (!minimal) p.push(at(side * outer, el(outer) - tick));
          d += pts(p);
        } else if (!boeing && !fighter && !minimal && lw === 1) {  // the Airbus dashes
          for (j = 0; j < 4; j++) {
            a0 = inner + j * 19.7 / D; a1 = a0 + 12 / D;
            p = [at(side * a0, e), at(side * a1, e)];
            if (j === 3) p.push(at(side * a1, e + tick));
            d += pts(p);
          }
        } else {
          for (j = 0; j < dashes; j++) {
            a0 = inner + j * dl * 1.64; a1 = a0 + dl;
            p = range(a0, a1, 3).map(function (a) { return at(side * a, el(a)); });
            if (j === dashes - 1 && !minimal) p.push(at(side * a1, el(a1) + tick));
            d += pts(p);
          }
        }
        if (!minimal) {
          var la = outer + 2.5 / D, q = at(side * la, fighter ? el(la) : e);
          lab += TX(f1(q[0]), f1(q[1] + 9), String(Math.round(e * 10) / 10), 21, side < 0 ? 'end' : 'start');
        }
      }
    }
    return (d ? P(d) : '') + lab;
  }
  function hdgFixedSvg(s) {  // heading_fixed(): a compass arc low in the view, or a tape across the top
    var hdg = PV.hdg, h, d, x, ten, path = '', txt = '', half;
    function label(v) { return String(Math.floor((((v % 360) + 360) % 360) / 10)); }
    if (s.sty_hdg === 'arc') {
      var cy = 1010, R = 230;
      half = 35;
      var at = function (dd, r) { return [CX + r * Math.sin(dd / DEG), cy - r * Math.cos(dd / DEG)]; };
      path = pts(range(-half, half, 36).map(function (dd) { return at(dd, R); }));
      for (h = Math.floor((hdg - half) / 5) * 5; h <= Math.floor(hdg + half) + 1; h += 5) {
        d = h - hdg;
        if (Math.abs(d) > half) continue;
        ten = h % 10 === 0;
        path += pts([at(d, R), at(d, R - (ten ? 15 : 8))]);
        if (ten && Math.abs(d) > 4) { var q = at(d, R - 38); txt += TXr(q[0], q[1] + 8, label(h), 24, d); }
      }
      path += pts([[CX, cy - R - 3], [CX - 9, cy - R - 18], [CX + 9, cy - R - 18]], true);
      return P(path) + txt + TX(CX, cy - R - 26, '071', 26);
    }
    var y0 = 95, ppd = 6;
    half = 26;
    path = 'M' + (CX - half * ppd) + ' ' + y0 + 'H' + (CX + half * ppd);
    for (h = Math.floor((hdg - half) / 5) * 5; h <= Math.floor(hdg + half) + 1; h += 5) {
      d = h - hdg;
      if (Math.abs(d) > half) continue;
      x = CX + d * ppd; ten = h % 10 === 0;
      path += 'M' + f1(x) + ' ' + y0 + 'V' + (y0 + (ten ? 14 : 8));
      if (ten && Math.abs(x - CX) > 36) txt += TX(f1(x), y0 - 8, label(h), 22);
    }
    path += pts([[CX - 8, y0 + 28], [CX, y0 + 17], [CX + 8, y0 + 28]]) + readoutBox(CX - 31, CX + 31, y0 - 22, 15, 0, 0);
    return P(path) + txt + TX(CX, y0 - 13, '071', 24);
  }
  function speedSvg(s) {  // speed_tape(): airbus / boeing (box pointing at the tape) / box only
    var box = s.sty_speed === 'box', boeing = s.sty_speed === 'boeing', sp = tapeSpan(s.spd_len), top = sp[0], bot = sp[1];
    var d = '', t = '', v, y, ias = PV.ias;
    if (!box) {
      d = 'M140.9 ' + f1(top) + 'V' + f1(bot) + 'M68.1 ' + f1(top) + 'H161M68.1 ' + f1(bot) + 'H161';
      for (v = Math.max(30, Math.floor((ias - 45) / 10) * 10); v < ias + 46; v += 10) {
        y = 510.5 - (v - ias) * 4.25;
        if (!(top + 4 < y && y < bot - 4)) continue;
        d += 'M128.9 ' + f1(y) + 'H140.9';
        if (v % 20 === 0 && top + 14 < y && y < bot - 6 && !(boeing && Math.abs(y - 510.5) < 32)) t += TX(94, f1(y + 9.3), String(v), 26);
      }
    }
    if (box || boeing) {
      var x0 = box ? 50 : 56, x1 = box ? 136 : 126;
      return P(d + readoutBox(x0, x1, 510.5, 21, boeing ? 1 : 0, 12)) + t + TX((x0 + x1) / 2, 522, String(ias), 32);
    }
    return P(d) + '<path class="solid" d="M119.5 509.5V512.6H147.3L160.6 517.5V504.6L147.3 509.5Z"/>' + t;
  }
  function altSvg(s) {  // altitude_tape() + the baro setting + misc() RA: airbus (drum) / boeing / box only
    var box = s.sty_alt === 'box', boeing = s.sty_alt === 'boeing', sp = tapeSpan(s.alt_len), top = sp[0], bot = sp[1];
    var d = '', t = '', v, y, alt = PV.alt, by0 = boeing ? -1e9 : 480.1, by1 = boeing ? -1e9 : 540.9, lift = 692.13 - bot;
    if (!box) {
      d = 'M1057.4 ' + f1(bot) + 'H1133.2M1113.1 ' + f1(top) + 'V' + f1(bot) + 'M1057.4 ' + f1(top) + 'H1133.2';
      for (v = Math.floor((alt - 650) / 100) * 100; v < alt + 651; v += 100) {
        y = 510.5 - (v - alt) * 0.32;
        if (!(top + 4 < y && y < bot - 4) || (by0 - 4 < y && y < by1 + 4)) continue;
        d += 'M1104.6 ' + f1(y) + 'H1113.1';
        if (((v % 500) + 500) % 500 === 0 && top + 14 < y && y < bot - 6 && !(by0 - 12 < y && y < by1 + 18))
          t += TX(1081, f1(y + 8.5), v >= 0 ? String(Math.floor(v / 100)) : '-' + Math.floor(-v / 100), 26);
      }
    }
    if (box || boeing) {
      var x0 = box ? 1050 : 1124, x1 = box ? 1150 : 1206;
      d += readoutBox(x0, x1, 510.5, 21, boeing ? -1 : 0, 11);
      t += TX((x0 + x1) / 2, 522, String(Math.round(alt / 20) * 20), 30);
    } else {
      d += 'M1113.1 529.6H1057.3V491.4H1113.1V480.1H1150.8V540.9H1113.1V529.6';
      t += TX(1110, 522, '3', 34, 'end') + TX(1132, 520, '40', 26);
    }
    t += box ? TX(1100, 592.5, '1013', 26) + TX(1100, 624.5, 'RA 150', 26)
             : TX(1095, f1(730 - lift), '1013', 26) + TX(1095, f1(772 - lift), 'RA 150', 26);
    return P(d) + t;
  }
  function vsSvg(s) {  // vertical_speed(): the needle, or a number under the altitude readout
    if (s.sty_vs === 'digital') return TX(s.sty_alt === 'boeing' ? 1165 : s.sty_alt === 'box' ? 1100 : 1104, 560.5, 'VS −700', 22);
    if (s.xwind_mode) return '';
    var yo = 95.27;  // -700 fpm
    return '<path class="solid" d="M1165 508.5H1185V512.5H1165Z"/>' + P('M1220 ' + f1(510.5 + 0.26 * yo) + 'L1183 ' + f1(510.5 + yo), 'stroke', 'stroke-width="4"') +
      TX(1192, f1(518 + yo + 14), '-07', 22);
  }
  function rollSvg(s) {  // roll_scale(), wings level
    var cx = 640, cy = 329.143, d;
    function at(a, r) { var t = (90 + a) / DEG; return [cx + r * Math.cos(t), cy - r * Math.sin(t)]; }
    function ticks(list, len, R, f) { var o = ''; list.forEach(function (a) { o += pts([f(a, R), f(a, R + len(a))]); }); return o; }
    var index = pts([[640, 138.44], [652.22, 118.24], [627.79, 118.24]], true);
    if (s.sty_roll === 'boeing')
      return P(ticks([-60, -45, -30, -20, -10, 10, 20, 30, 45, 60], function (a) { return Math.abs(a) === 30 || Math.abs(a) === 60 ? 24 : 13; }, 190, at) + index +
        pts([[640, 144], [630, 162], [650, 162]], true) + pts([[630, 166], [650, 166], [650, 174], [630, 174]], true));
    if (s.sty_roll === 'bottom') {
      var bx = 640, by = 760, R = 110;
      var bat = function (a, r) { var t = (a - 90) / DEG; return [bx + r * Math.cos(t), by - r * Math.sin(t)]; };
      d = pts(range(-60, 60, 41).map(function (a) { return bat(a, R); }));
      d += ticks([-60, -45, -30, -20, -10, 0, 10, 20, 30, 45, 60], function (a) { return Math.abs(a) === 30 || Math.abs(a) === 60 || a === 0 ? 18 : 10; }, R, bat);
      return P(d + pts([[bx, by + R - 4], [bx - 9, by + R - 21], [bx + 9, by + R - 21]], true) +
        pts([[bx - 9, by + R - 26], [bx + 9, by + R - 26], [bx + 9, by + R - 32], [bx - 9, by + R - 32]], true));
    }
    var pointer = pts([[640.18, 140], [630.18, 161.89], [650.18, 161.89]], true) + pts([[629, 164.85], [621, 181], [659, 181.07], [651, 164.85]], true);
    if (s.sty_roll === 'airbus') {  // the A320 HUD: a short arc, ticks at 10, 20, 30 and 45
      d = pts(range(-45, 45, 31).map(function (a) { return at(a, 190); }));
      d += ticks([-45, -30, -20, -10, 10, 20, 30, 45], function (a) { return Math.abs(a) === 10 || Math.abs(a) === 20 ? 13 : 24; }, 190, at);
      return P(d + index + pointer);
    }
    d = '';  // the FlyByWire (PFD) geometry
    for (var j = 0; j <= 39; j++) {
      var a = (155.16 + (24.84 - 155.16) * j / 39) / DEG;
      d += (j ? 'L' : 'M') + f1(640 + 190 * Math.cos(a)) + ' ' + f1(329.1 - 190 * Math.sin(a));
    }
    d += 'M735.2 164L749.3 139.5M705.1 150L712.8 129.3M673 141.5L677 119.3M544.8 164L530.7 139.5M574.9 150L567.2 129.3M607 141.5L603 119.3M774.5 194.5L794.5 174.5M505.5 194.5L485.5 174.5M813 249L832 240M467 249L448 240';
    return P(d + index + pointer);
  }
  function attSvg(s) {  // aircraft_ref(): wings + square, or the fighter's W with that bird
    if (s.sty_fpv === 'fighter') return P('M596 332L614 332L626 348L640 334L654 348L666 332L684 332', 'stroke', 'stroke-width="3.4"');
    return P('M625 335V329H595M637 332H643V326H637ZM655 335V329H685', 'stroke', 'stroke-width="3.4"');
  }
  function birdShape(s, y) {  // the flight path symbol (scene.cpp sty_fpv): circle cr, wings to `wing`, tail to `tail`
    var r = 17 * (s.bird_size || 1), st = s.sty_fpv, cr = r, wing = 3 * r, tail = 2.1 * r, wings = true, x = CX, d, extra = '', k, a;
    if (st === 'boeing') { cr = 0.9 * r; wing = 3.3 * r; tail = 1.9 * r; }
    else if (st === 'fighter') { cr = 0.8 * r; wing = 2.5 * r; tail = 2.0 * r; }
    else if (st === 'minimal') { cr = 0.85 * r; wing = 2.0 * r; tail = 0; }
    else if (st === 'ring') { wing = 1.6 * r; tail = 0; wings = false; }
    else if (st === 'cross') { cr = 0.7 * r; wing = 1.7 * r; tail = 0; wings = false; }
    d = 'M' + f1(x - cr) + ' ' + f1(y) + 'a' + f1(cr) + ' ' + f1(cr) + ' 0 1 0 ' + f1(2 * cr) + ' 0a' + f1(cr) + ' ' + f1(cr) + ' 0 1 0 ' + f1(-2 * cr) + ' 0';
    if (wings) d += 'M' + f1(x - cr) + ' ' + f1(y) + 'H' + f1(x - wing) + 'M' + f1(x + cr) + ' ' + f1(y) + 'H' + f1(x + wing);
    if (tail > 0) d += 'M' + x + ' ' + f1(y - cr) + 'V' + f1(y - tail);
    if (st === 'cross') for (k = 0; k < 4; k++) {
      a = k * Math.PI / 2;
      d += 'M' + f1(x + cr * Math.cos(a)) + ' ' + f1(y + cr * Math.sin(a)) + 'L' + f1(x + 1.7 * r * Math.cos(a)) + ' ' + f1(y + 1.7 * r * Math.sin(a));
    }
    if (st === 'ring') extra = '<circle class="solid" cx="' + x + '" cy="' + f1(y) + '" r="' + f1(0.16 * r) + '"/>';
    return { d: d, extra: extra, r: r, cr: cr, wing: wing, tail: tail };
  }
  function watermarkSvg(s) {  // test builds (version with a suffix): over the altitude tape, as the program draws it
    var v = S.st && S.st.version ? String(S.st.version) : '';
    if (v.indexOf('-') < 0) return '';
    return TX(1095, f1(tapeSpan(s.alt_len)[0] - 18), S.lang === 'zh' ? 'Birdie 测试版' : 'Birdie beta', 22);
  }

  function build() {
    var s = S.set, out = [], h, k, y, e, j, side;
    var glide = s.glide_deg, ladder = s.ladder_step_deg, scale = s.hud_scale;
    var rw = (S.st && S.st.runway) ? S.st.runway.name : '07L';
    rwSetup(s.tdz_rule === 'faa');
    var top = rwTop(), bot = rwBottom(), farHalf = runwayHalf(top);
    // ---- conformal: stays with the world
    out.push('<g class="el" data-el="horizon">' + P('M-40 ' + HY + 'H' + f1(CX - farHalf - 6) + 'M' + f1(CX + farHalf + 6) + ' ' + HY + 'H1320') + '</g>');
    var hd = '', hl = '';
    if (s.sty_hdg !== 'arc' && s.sty_hdg !== 'tape') {  // marks on the horizon (the arc and the tape: airframe-fixed, below)
      for (h = 50; h <= 95; h += 5) {
        var x = CX + (h - 71) * D, ten = h % 10 === 0;
        hd += 'M' + f1(x) + ' ' + HY + 'v' + (ten ? 14 : 8);
        if (ten) hl += TX(f1(x), HY + 40, String(h / 10), 21);
      }
      out.push('<g class="el" data-el="hdg">' + P(hd, 'stroke thin') + hl + '</g>');
    }
    out.push('<g class="el" data-el="ladder">' + ladderSvg(s) + '</g>');
    var mn = '';
    for (j = -12; j <= 12; j++) {
      e = j * 2.5; y = HY - e * D;
      if (!j || y < 70 || y > 870 || (ladder > 0 && Math.abs(e / ladder - Math.round(e / ladder)) < 1e-6)) continue;
      if (!(y > top && y < bot + 20)) mn += 'M' + (CX - 12) + ' ' + f1(y) + 'H' + (CX + 12);  // clipped across the runway
    }
    out.push('<g class="el" data-el="minor">' + (mn ? P(mn, 'stroke thin') : '') + '</g>');
    // runway outline + touchdown zone marks (aiming point stripes; all bars with "full"; the zone's end as two ticks)
    var hw = RW.w / 2, rwp = rwQuad([0, RW.len, -hw, hw]), mk = '', mf = '';
    if (s.show_tdz) {
      var tm = rwMarks(s.tdz_rule === 'faa');
      if (s.tdz_style !== 'end') tm.aim.forEach(function (q) { mk += rwQuad(q); });
      if (s.tdz_style === 'full') tm.bars.forEach(function (q) { mk += rwQuad(q); });
      mf = rwQuad([tm.end, tm.end + 4, -hw - 10, -hw + 6]) + rwQuad([tm.end, tm.end + 4, hw - 6, hw + 10]);
    }
    out.push('<g class="el" data-el="runway">' + P(rwp) + (mk ? P(mk, 'stroke thin') : '') + (mf ? '<path class="solid" d="' + mf + '"/>' : '') +
      TX(CX, f1(bot + 30), rw, 20) + '</g>');
    // glide line: span grows with size, cut where it crosses the runway
    var gy = HY + glide * D, span = Math.min(18, 10 * scale) * D;
    var gap = (gy > top && gy < bot) ? runwayHalf(gy) + 4 : 0;
    var gl = gap ? 'M' + (CX - span) + ' ' + f1(gy) + 'H' + (CX - gap) + 'M' + (CX + gap) + ' ' + f1(gy) + 'H' + (CX + span)
                 : 'M' + (CX - span) + ' ' + f1(gy) + 'H' + (CX + span);
    out.push('<g class="el" data-el="glide"><path class="stroke dash" d="' + gl + '"/>' + TX(CX + span + 6, f1(gy + 8), '-' + glide.toFixed(1), 20, 'start') + '</g>');
    // bird (floats a little) + numbers + flare cue, placed off its shape as scene.cpp does
    var by = rwAt(rwAim, 0)[1], b = birdShape(s, by), r = b.r;  // = 3° down: on the aiming point
    var arm = 0.55 * r, fy = by - (b.tail > 0 ? b.tail : b.cr + 0.4 * r) - 12 - arm, fl = '';
    [CX - arm - 7, CX + arm + 7].forEach(function (xc) {
      fl += 'M' + f1(xc - arm) + ' ' + f1(fy) + 'H' + f1(xc + arm) + 'M' + f1(xc) + ' ' + f1(fy - arm) + 'V' + f1(fy + arm);
    });
    out.push('<g id="birdfloat">' +
      '<g class="el" data-el="bird">' + P(b.d) + b.extra + '</g>' +
      '<g class="el" data-el="fpa">' + TX(f1(CX + b.wing + 10), f1(by + 9), '\u22123.0', 22, 'start') + '</g>' +
      '<g class="el" data-el="vsn">' + TX(f1(CX - b.wing - 10), f1(by + 9), '\u2212700', 22, 'end') + '</g>' +
      '<g class="el" data-el="bra">' + TX(CX, f1(by + r + 30), '150', 22) + '</g>' +
      '<g class="el" data-el="flare"><g id="flare" opacity="0">' + P(fl) + '</g></g></g>');
    // ---- airframe-fixed: scales with size around the boresight
    // crosswind mode (scene.cpp kXw*): tapes 55 % as tall (tapeSpan), moved out (gx), no VS needle
    var fx = [];
    fx.push('<g class="el" data-el="speed"' + gx('spd') + '>' + speedSvg(s) + '</g>');
    fx.push('<g class="el" data-el="gs"' + gx('wind') + '>' + TX(68, 300, 'GS 142', 24, 'start') + TX(298, 258, '264', 22, 'end') +
      TX(306, 257, '/', 20) + TX(315, 258, '8', 22, 'start') + P('M276 298V268M269.5 280L276 268L282.5 280', 'stroke', 'transform="rotate(193 276 283)"') + '</g>');
    fx.push('<g class="el" data-el="alt"' + gx('alt') + '>' + altSvg(s) + '</g>');
    fx.push('<g class="el" data-el="wm"' + gx('alt') + '>' + watermarkSvg(s) + '</g>');
    fx.push('<g class="el" data-el="vs"' + gx('vs') + '>' + vsSvg(s) + '</g>');
    fx.push('<g class="el" data-el="roll"' + gx('roll') + '>' + rollSvg(s) + '</g>');
    fx.push('<g class="el" data-el="att"' + gx('att') + '>' + attSvg(s) + '</g>');
    if (s.sty_hdg === 'arc' || s.sty_hdg === 'tape') fx.push('<g class="el" data-el="hdg"' + gx('hdg') + '>' + hdgFixedSvg(s) + '</g>');
    out.push('<g id="fixed" transform="' + fixedTransform(scale) + '">' + fx.join('') + '</g>');
    return out.join('');
  }
  // the layout settings, as the program applies them to each group (hud/scene.cpp groups())
  function gx(n) {
    var s = S.set, D = 36.5714, sp = s.lay_spread - 1, xw = !!s.xwind_mode;
    var sideS = (xw ? -50 : 0) - sp * (640 - 140.882), sideA = (xw ? 60 : 0) + sp * (1113.112 - 640);
    var t = { spd: [sideS + s.spd_dx * D, s.spd_dy * D, s.spd_size, 140.882, 510.5],
              alt: [sideA + s.alt_dx * D, s.alt_dy * D, s.alt_size, 1113.112, 510.5],
              wind: [(xw ? -50 : 0) + s.wind_dx * D, s.wind_dy * D, s.wind_size, 68, 300],
              roll: [0, s.roll_dy * D, s.roll_size, 640, 329.143], att: [0, 0, s.att_size, 640, 332],
              hdg: [0, (s.hdg_dy || 0) * D, 1, 640, 329.143] };
    t.vs = [t.alt[0] + s.vs_dx * D, t.alt[1] + s.vs_dy * D, s.vs_size, 1185, 510.5];
    var a = t[n];
    if (!a[0] && !a[1] && a[2] === 1) return '';
    return ' transform="translate(' + (a[0] + a[3]).toFixed(1) + ' ' + (a[1] + a[4]).toFixed(1) + ') scale(' + a[2] + ') translate(' + (-a[3]) + ' ' + (-a[4]) + ')"';
  }
  function fixedTransform(k) { return 'translate(' + CX + ' ' + BORE + ') scale(' + k.toFixed(4) + ') translate(' + (-CX) + ' ' + (-BORE) + ')'; }

  var hud = null, shownScale = 1, scaleAnim = null, builtKey = '';
  function render() {
    var s = S.set, key = [s.glide_deg, s.ladder_step_deg, s.xwind_mode, S.st && S.st.runway ? S.st.runway.name : '', s.bird_size, s.lay_spread,
      s.show_tdz, s.tdz_rule, s.tdz_style,
      s.spd_dx, s.spd_dy, s.spd_size, s.alt_dx, s.alt_dy, s.alt_size, s.vs_dx, s.vs_dy, s.vs_size, s.wind_dx, s.wind_dy, s.wind_size,
      s.roll_dy, s.roll_size, s.att_size, s.spd_len, s.alt_len, s.hdg_dy, s.ladder_w, s.ladder_max_deg,
      s.sty_fpv, s.sty_speed, s.sty_alt, s.sty_vs, s.sty_ladder, s.sty_hdg, s.sty_roll, S.lang, S.st && S.st.version].join('|');
    if (key !== builtKey) {  // rebuild only for what changes geometry; the rest is classes and attributes
      builtKey = key;
      hud.innerHTML = build();
      lights();  // the runway moves a little with the aiming point rule (ICAO 425 m, FAA 334 m in)
      shownScale = s.hud_scale;
    }
    apply();
  }
  function apply() {
    var s = S.set;
    hud.setAttribute('opacity', Math.max(PREVIEW_MIN_OPACITY, s.opacity).toFixed(2));
    hud.style.color = !s.hud_color || s.hud_color === '#00ff00' ? '#57f27f' : s.hud_color;  // the preview's green is softer
    cls(hud, 'spot', !!S.focus);
    each(hud.querySelectorAll('.el'), function (el) {
      var k = el.getAttribute('data-el'), g = glyph(k);
      cls(el, 'off', g ? !s[g[1]] : false);
      cls(el, 'focus', S.focus === k);
    });
    var fl = $('flare');
    if (fl) { var on = S.focus === 'flare' && s.show_flare; cls(fl, 'blink', on); fl.setAttribute('opacity', on ? '1' : '0'); }
    tweenScale(s.hud_scale);
  }
  function tweenScale(target) {  // the instruments breathe to their new size
    var fixed = $('fixed');
    if (!fixed || Math.abs(target - shownScale) < 1e-4) { if (fixed) fixed.setAttribute('transform', fixedTransform(target)); shownScale = target; return; }
    var from = shownScale, t0 = Date.now(), dur = 500;
    if (scaleAnim) clearInterval(scaleAnim);
    scaleAnim = setInterval(function () {
      var t = Math.min(1, (Date.now() - t0) / dur), e = 1 - Math.pow(1 - t, 3);
      shownScale = from + (target - from) * e;
      var f = $('fixed'); if (f) f.setAttribute('transform', fixedTransform(shownScale));
      if (t >= 1) { clearInterval(scaleAnim); scaleAnim = null; }
    }, 16);
  }
  // class helper that also works on SVG elements in older engines
  function cls(el, c, on) {
    var cur = (el.getAttribute('class') || '').split(/\s+/).filter(function (x) { return x && x !== c; });
    if (on) cur.push(c);
    el.setAttribute('class', cur.join(' '));
  }
  function hasCls(el, c) { return (' ' + (el.getAttribute('class') || '') + ' ').indexOf(' ' + c + ' ') >= 0; }

  // runway approach lights: a few warm points along the edges
  function lights() {
    var c = '';
    for (var i = 0; i <= 9; i++) {  // evenly along the runway, so they bunch up towards the far end as real ones do
      var t = i / 9, p = rwAt(RW.len * (1 - t), RW.w / 2), y = p[1], h = p[0] - CX + 3, r = (1 + t * 2.2).toFixed(1);
      c += '<circle cx="' + f1(CX - h) + '" cy="' + f1(y) + '" r="' + r + '" style="animation-delay:' + (i * 0.37).toFixed(2) + 's"/>' +
           '<circle cx="' + f1(CX + h) + '" cy="' + f1(y) + '" r="' + r + '" style="animation-delay:' + (i * 0.37 + 0.2).toFixed(2) + 's"/>';
    }
    $('lights').innerHTML = c;
  }

  // ---------------------------------------------------------------- preview: only while the real HUD is away
  var pvShown = null;
  function hudOnScreen() { return S.online && !!(S.st && S.st.showing); }
  function preview() {
    var want = !PEEK && !hudOnScreen(), l = L();
    $('pv-l').textContent = !S.online ? l.pvOffline : (S.set.visible ? l.pvAway : l.pv);
    $('pv-r').textContent = S.set.opacity < PREVIEW_MIN_OPACITY - 1e-6
      ? fmt(l.pvDim, { min: Math.round(PREVIEW_MIN_OPACITY * 100), v: Math.round(S.set.opacity * 100) }) : '';
    if (want === pvShown) return;
    pvShown = want;
    var w = $('pvwrap');
    cls($('pocket'), 'pvon', want);
    w.style.height = (want ? $('pv').offsetHeight : 0) + 'px';
    fitHost();
  }

  // ---------------------------------------------------------------- pages
  var HOLD_PAGES = [5, 6, 7, 8], holding = false;
  function hold() {  // the HUD stays on (even on the ground) while a look page is open; renewed every 8 s
    var on = HOLD_PAGES.indexOf(S.page) >= 0 && S.online;
    if (on || holding) act('hold', on);
    holding = on;
  }
  function pageList() { return document.querySelectorAll('.pages > .page'); }
  function layoutPages() {
    var all = pageList(), n = all.length;
    $('panel').querySelector('.pages').style.width = (n * 100) + '%';
    each(all, function (pg) { pg.style.width = (100 / n) + '%'; });
  }
  function go(p) {
    S.page = p;
    $('panel').setAttribute('data-p', String(p));
    var all = pageList(), ix = 0;
    each(all, function (pg, i) { var me = pg.id === 'page' + p; cls(pg, 'cur', me); if (me) ix = i; });
    $('panel').querySelector('.pages').style.transform = 'translateX(' + (-100 * ix / all.length) + '%)';
    hold();
    if (p !== 9 && listening) listen(null);
    if (p !== 1) focusOn(null);
    fitPanel();
    // keyboard users keep their place; focusing never scrolls the sliding pages (overflow: hidden still scrolls)
    var first = $('page' + p).querySelector('[role="button"]');
    if (first && document.activeElement && document.activeElement !== document.body) {
      try { first.focus({ preventScroll: true }); } catch (e) { first.focus(); }
    }
    $('panel').scrollLeft = 0;
    setTimeout(function () { $('panel').scrollLeft = 0; }, 0);
  }
  function fitPanel() {
    var page = $('page' + S.page);
    if (page) $('panel').style.height = page.offsetHeight + 'px';
    fitHost();
  }
  // Birdie's own window follows the page's height: at once when growing, after the slide when shrinking
  var hostH = 0, shrinkTimer = null;
  function fitHost() {
    if (!HOST) return;
    var page = PEEK ? $('peekcard') : $('page' + S.page), pv = pvShown ? $('pv').offsetHeight : 0;
    var h = Math.ceil((page ? page.offsetHeight : 0) + pv);  // css px (1rem = 14 px in the window)
    clearTimeout(shrinkTimer);
    if (h >= hostH || PEEK) { hostH = h; host({ host: 'size', h: h }); }  // the strip has no slide to wait for
    else shrinkTimer = setTimeout(function () { hostH = h; host({ host: 'size', h: h }); }, 820);
  }

  // ---------------------------------------------------------------- symbol switches
  function focusOn(k) {
    if (S.focus === k) return;
    S.focus = k;
    apply();
    var g = k && glyph(k);
    send({ type: 'focus', key: g ? g[1] : null });
    describe(k);
  }
  function glyphs() {
    for (var group in GLYPHS) {
      var html = '';
      GLYPHS[group].forEach(function (g) {
        html += '<div class="g" role="button" tabindex="0" data-k="' + g[0] + '"><svg viewBox="0 0 34 34">' + g[6] + '</svg><span class="nm">' +
          (S.lang === 'zh' ? g[2] : g[3]) + '</span></div>';
      });
      $('g-' + group).innerHTML = html;
    }
    each(document.querySelectorAll('.g'), function (b) {
      var k = b.getAttribute('data-k'), g = glyph(k);
      press(b, function () { set(g[1], !S.set[g[1]]); paint(); describe(k, true); });
      var enter = function () { focusOn(k); };
      var leave = function () { focusOn(null); };
      b.addEventListener('mouseenter', enter); b.addEventListener('focus', enter);
      b.addEventListener('mouseleave', leave); b.addEventListener('blur', leave);
    });
  }
  var descTimer = null;
  function describe(k, now) {
    var box = $('gdesc');
    var put = function () {
      var l = L();
      if (!k) box.innerHTML = l.gIdle;
      else {
        var g = glyph(k), on = S.set[g[1]];
        box.innerHTML = '<b>' + (S.lang === 'zh' ? g[2] : g[3]) + '</b>' + (S.lang === 'zh' ? g[4] : g[5]) + (on ? '' : (S.lang === 'zh' ? '。' : '. ') + l.nowOff);
      }
      box.style.opacity = 1;
    };
    clearTimeout(descTimer);
    if (now) { put(); return; }
    box.style.opacity = 0;
    descTimer = setTimeout(put, 200);
  }

  // ---------------------------------------------------------------- scales (own slider: Coherent has no range input)
  var SCALES = {
    bright: { key: 'opacity', min: 25, max: 100, step: 5, get: function (v) { return Math.round(v * 100); }, put: function (x) { return x / 100; }, out: function (x) { return x + '%'; } },
    size:   { key: 'hud_scale', min: 60, max: 115, step: 5, get: function (v) { return Math.round(v * 100); }, put: function (x) { return x / 100; }, out: function (x) { return x + '%'; } },
    glide:  { key: 'glide_deg', min: 2.0, max: 5.9, step: 0.1, get: function (v) { return v; }, put: function (x) { return Math.round(x * 10) / 10; }, out: function (x) { return x.toFixed(1) + '°'; } },
    outline: pct('hud_outline', 0, 1.5), glow: pct('hud_glow', 0, 1),
    trStrength: { key: 'terrain_strength', min: 20, max: 200, step: 5, get: function (v) { return Math.round(v * 100); }, put: function (x) { return x / 100; }, out: function (x) { return x + '%'; } }
  };
  function pct(key, min, max) {
    return { key: key, min: min, max: max, step: 0.05, get: function (v) { return v; }, put: function (x) { return Math.round(x * 100) / 100; },
             out: function (x) { return Math.round(x * 100) + '%'; } };
  }
  function degs(key) {
    return { key: key, min: -8, max: 8, step: 0.1, get: function (v) { return v; }, put: function (x) { return Math.round(x * 10) / 10; },
             out: function (x) { return (x > 0.05 ? '+' : '') + x.toFixed(1) + '°'; } };
  }
  // the layout page: [heading, [[setting, label, kind (pct with its range, or deg)]...]]
  var LAYOUT = [
    ['g_all', [['lay_spread', 'l_spread', 0.6, 1.4], ['text_size', 'l_text', 0.7, 1.5], ['line_w', 'l_line', 0.5, 2]]],
    ['g_spd', [['spd_dx', 'l_dx'], ['spd_dy', 'l_dy'], ['spd_len', 'l_len', 0.4, 1.6], ['spd_size', 'l_sz', 0.6, 1.6]]],
    ['g_alt', [['alt_dx', 'l_dx'], ['alt_dy', 'l_dy'], ['alt_len', 'l_len', 0.4, 1.6], ['alt_size', 'l_sz', 0.6, 1.6]]],
    ['g_vs', [['vs_dx', 'l_dx'], ['vs_dy', 'l_dy'], ['vs_size', 'l_sz', 0.6, 1.6]]],
    ['g_wind', [['wind_dx', 'l_dx'], ['wind_dy', 'l_dy'], ['wind_size', 'l_sz', 0.6, 1.6]]],
    ['g_roll', [['roll_dy', 'l_dy'], ['roll_size', 'l_sz', 0.6, 1.6]]],
    ['g_misc', [['bird_size', 'l_bird', 0.5, 2], ['att_size', 'l_att', 0.5, 2], ['ladder_w', 'l_ladder', 0.5, 2], ['hdg_dy', 'l_hdg']]]
  ];
  function buildLayout() {  // before scales(): one slider box per layout setting, under its group's heading
    var html = '';
    LAYOUT.forEach(function (g) {
      html += '<div class="gh"><span id="t-' + g[0] + '"></span><i></i></div>';
      g[1].forEach(function (r) {
        var id = 'L_' + r[0];
        SCALES[id] = r.length > 2 ? pct(r[0], r[2], r[3]) : degs(r[0]);
        html += '<div class="scale wide" id="s-' + id + '"></div>';
      });
    });
    $('lay-list').innerHTML = html;
  }
  function layoutWords() {
    var l = L();
    LAYOUT.forEach(function (g) {
      $('t-' + g[0]).textContent = l[g[0]];
      g[1].forEach(function (r) { $('t-L_' + r[0]).textContent = l[r[1]]; });
    });
  }
  // ---------------------------------------------------------------- hotkeys (app/hotkeys.h)
  var HOTKEYS = [['k_hud', ['toggle', 'xwind', 'preset_next', 'anim_test', 'panel', 'quit']],
                 ['k_tune', ['glide_up', 'glide_down', 'lead_up', 'lead_down', 'bright_up', 'bright_down', 'size_up', 'size_down']]];
  var listening = null, listenTimer = null, badUntil = 0, listenAt = 0;
  // keyCode -> the name Birdie reads (parse_combo); modifiers alone are not keys
  function keyName(c) {
    var fixed = { 45: 'Insert', 46: 'Delete', 36: 'Home', 35: 'End', 33: 'PgUp', 34: 'PgDn', 38: 'Up', 40: 'Down', 37: 'Left', 39: 'Right',
                  32: 'Space', 107: 'Num+', 109: 'Num-', 106: 'Num*', 111: 'Num/' };
    if (fixed[c]) return fixed[c];
    if ((c >= 65 && c <= 90) || (c >= 48 && c <= 57)) return String.fromCharCode(c);
    if (c >= 112 && c <= 135) return 'F' + (c - 111);
    if (c >= 96 && c <= 105) return 'Num' + (c - 96);
    return null;
  }
  function buildKeys() {
    var html = '';
    HOTKEYS.forEach(function (g) {
      html += '<div class="gh"><span id="t-' + g[0] + '"></span><i></i></div>';
      g[1].forEach(function (a) {
        html += '<div class="hkrow"><span id="t-k_' + a + '"></span><span class="key" role="button" tabindex="0" data-a="' + a + '"></span></div>';
      });
    });
    $('hk-list').innerHTML = html;
    each($('hk-list').querySelectorAll('.key'), function (el) {
      press(el, function () { if (listening !== el.getAttribute('data-a')) listen(el.getAttribute('data-a')); });
    });
  }
  function keysWords() {
    var l = L();
    HOTKEYS.forEach(function (g) {
      $('t-' + g[0]).textContent = l[g[0]];
      g[1].forEach(function (a) { $('t-k_' + a).textContent = l['k_' + a]; });
    });
  }
  function listen(a) {  // Birdie lets go of its hotkeys meanwhile, so a combo it holds reaches the page too
    listening = a;
    badUntil = 0;
    listenAt = Date.now();
    act('hotkey_capture', !!a);
    clearTimeout(listenTimer);
    if (a) listenTimer = setTimeout(function () { listen(null); }, 15000);
    paintKeys();
  }
  function onListenKey(e) {
    if (!listening) return false;
    e.preventDefault();
    if (e.stopPropagation) e.stopPropagation();
    var c = e.keyCode;
    if (Date.now() - listenAt < 200) return true;  // the Enter / Space that opened it
    if (c === 16 || c === 17 || c === 18 || c === 91 || c === 92) return true;  // still holding the modifiers
    var mods = (e.ctrlKey ? 'Ctrl+' : '') + (e.altKey ? 'Alt+' : '') + (e.shiftKey ? 'Shift+' : '') + (e.metaKey ? 'Win+' : '');
    if (c === 27 && !mods) { listen(null); return true; }
    var a = listening, name = keyName(c);
    if ((c === 8 || c === 46) && !mods) { setKey(a, ''); listen(null); return true; }
    if (!name) return true;
    if (!mods && !/^F\d+$/.test(name)) { badUntil = Date.now() + 2500; paintKeys(); setTimeout(paintKeys, 2600); return true; }
    setKey(a, mods + name);
    listen(null);
    return true;
  }
  function setKey(a, combo) {
    var hk = S.set.hotkeys;
    if (combo) Object.keys(hk).forEach(function (k) { if (k !== a && hk[k] === combo) hk[k] = ''; });  // as Birdie does
    hk[a] = combo;
    touched.hotkeys = Date.now();
    send({ type: 'set', key: 'hotkey.' + a, value: combo });
    paint();
  }
  function paintKeys() {
    var l = L(), hk = S.set.hotkeys || {}, failed = (S.st && S.st.hotkey_failed) || [], n = 0;
    each($('hk-list').querySelectorAll('.key'), function (el) {
      var a = el.getAttribute('data-a'), v = hk[a] || '', me = listening === a, bad = me && Date.now() < badUntil;
      var taken = !me && v && failed.indexOf(a) >= 0;
      el.textContent = bad ? l.hkNeedMod : me ? l.hkListen : !v ? l.hkNone : taken ? v + ' · ' + l.hkTaken : v;
      cls(el, 'listen', me && !bad); cls(el, 'bad', bad); cls(el, 'none', !me && !v); cls(el, 'taken', !!taken);
      if (v) n++;
    });
    $('n-keys').textContent = fmt(l.keysN, { n: n });
    $('xw-hk').textContent = hk.xwind || '';
  }

  function scales() {
    Object.keys(SCALES).forEach(function (id) {
      var c = SCALES[id], box = $('s-' + id);
      box.innerHTML = '<div class="lbl" id="t-' + id + '"></div><div class="track ease" role="button" tabindex="0"><div class="rail"></div>' +
        '<div class="fill"></div><div class="thumb"></div></div><div class="out"></div>';
      var track = box.querySelector('.track'), sent = 0, last = null;
      var valueAt = function (clientX) {
        var r = track.getBoundingClientRect(), p = Math.max(0, Math.min(1, (clientX - r.left) / r.width));
        return Math.round((c.min + p * (c.max - c.min)) / c.step) * c.step;
      };
      var update = function (x, final) {
        x = Math.max(c.min, Math.min(c.max, Math.round(x / c.step) * c.step));
        var v = c.put(x);
        S.set[c.key] = v;
        touched[c.key] = Date.now();
        paintScale(id);
        if (id === 'glide') { render(); fact(); } else apply();
        preview();
        var now = Date.now();
        if (final || (v !== last && now - sent > 70)) { if (v !== last || final) send({ type: 'set', key: c.key, value: v }); sent = now; last = v; }
      };
      var move = function (e) { update(valueAt(e.clientX)); };
      var up = function (e) {
        document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up);
        cls(track, 'drag', false); cls(track, 'ease', true); update(valueAt(e.clientX), true);
      };
      track.addEventListener('mousedown', function (e) {
        e.preventDefault(); cls(track, 'drag', true); cls(track, 'ease', false); last = null; update(valueAt(e.clientX));
        document.addEventListener('mousemove', move); document.addEventListener('mouseup', up);
      });
      track.addEventListener('keydown', function (e) {
        if (COUI) return;  // in the sim the arrow keys fly the aircraft / move the view
        var d = (e.keyCode === 37 || e.keyCode === 40) ? -1 : (e.keyCode === 39 || e.keyCode === 38) ? 1 : 0;
        if (d) { e.preventDefault(); update(c.get(S.set[c.key]) + d * c.step, true); }
      });
    });
  }
  function paintScale(id) {
    var c = SCALES[id], box = $('s-' + id), x = c.get(S.set[c.key]);
    var p = Math.max(0, Math.min(1, (x - c.min) / (c.max - c.min))) * 100;
    box.querySelector('.fill').style.width = p.toFixed(2) + '%';
    box.querySelector('.thumb').style.left = p.toFixed(2) + '%';
    box.querySelector('.out').textContent = c.out(x);
  }

  // ---------------------------------------------------------------- fine tuning
  var ANIMS = ['none', 'fade', 'iris', 'boot', 'scan', 'assemble'];
  var COLORS = [['c_green', '#00ff00'], ['c_soft', '#3cff74'], ['c_lime', '#b6ff3c'], ['c_white', '#ffffff'], ['c_cyan', '#4de8ff'],
                ['c_amber', '#ffb000'], ['c_magenta', '#ff6ad5']];
  var FONTS = { bahnschrift: 'Bahnschrift', consolas: 'Consolas', segoe: 'Segoe UI', narrow: 'Arial Narrow' };
  var PICKS = {
    lead: { key: 'lead_ms', vals: [0, 20, 40, 60, 80, 100, 150], label: function (v) { return String(v); }, unit: ' ms' },
    tol: { key: 'flare_tol_deg', vals: [0.2, 0.3, 0.5, 0.8], label: function (v) { return '±' + v.toFixed(1) + '°'; }, unit: '' },
    ladder: { key: 'ladder_step_deg', vals: [0, 3, 5, 6, 10], label: function (v) { return v ? v + '°' : L().off; }, unit: '' },
    rah: { key: 'bird_ra_below_ft', vals: [200, 300, 500, 1000, 1500, 2500], label: function (v) { return String(v); }, unit: ' ft' },
    look: { key: 'ui_style', vals: STYLES, label: function (v) { return L()['look_' + v]; }, unit: '', str: true },
    askt: { key: 'auto_show_kt', vals: [20, 30, 40, 60, 80], label: function (v) { return String(v); }, unit: ' kt' },
    ahft: { key: 'auto_hide_ft', vals: [1000, 1500, 2500, 5000], label: function (v) { return String(v); }, unit: ' ft' },
    ahs: { key: 'auto_hide_s', vals: [10, 30, 60], label: function (v) { return String(v); }, unit: ' s' },
    // the HUD's look
    preset: { key: 'hud_preset', vals: ['airbus', 'boeing', 'fighter', 'minimal'], label: word('pre_'), unit: '', str: true, act: 'preset' },
    fpv: { key: 'sty_fpv', vals: ['airbus', 'boeing', 'fighter', 'minimal', 'ring', 'cross'], label: word('s_'), unit: '', str: true },
    spdS: { key: 'sty_speed', vals: ['airbus', 'boeing', 'box'], label: word('s_'), unit: '', str: true },
    altS: { key: 'sty_alt', vals: ['airbus', 'boeing', 'box'], label: word('s_'), unit: '', str: true },
    vsS: { key: 'sty_vs', vals: ['needle', 'digital'], label: word('s_'), unit: '', str: true },
    ladderS: { key: 'sty_ladder', vals: ['airbus', 'boeing', 'fighter', 'minimal'], label: word('s_'), unit: '', str: true },
    hdgS: { key: 'sty_hdg', vals: ['horizon', 'arc', 'tape'], label: word('s_'), unit: '', str: true },
    rollS: { key: 'sty_roll', vals: ['airbus', 'boeing', 'bottom'], label: word('s_'), unit: '', str: true },
    color: { key: 'hud_color', vals: COLORS.map(function (c) { return c[1]; }), label: swatch, unit: '', str: true },
    font: { key: 'hud_font', vals: ['bahnschrift', 'consolas', 'segoe', 'narrow'], label: function (v) { return FONTS[v]; }, unit: '', str: true },
    animIn: { key: 'anim_in', vals: ANIMS, label: word('a_'), unit: '', str: true },
    animOut: { key: 'anim_out', vals: ANIMS, label: word('a_'), unit: '', str: true },
    animTime: { key: 'anim_time', vals: [0.5, 0.75, 1, 1.5, 2], label: function (v) { return v + '×'; }, unit: '' },
    // approach
    tdzRule: { key: 'tdz_rule', vals: ['icao', 'faa'], label: word('t_'), unit: '', str: true },
    tdzStyle: { key: 'tdz_style', vals: ['aim', 'full', 'end'], label: word('t_'), unit: '', str: true },
    flareRa: { key: 'flare_ra_ft', vals: [30, 40, 50, 70, 100], label: function (v) { return String(v); }, unit: ' ft' },
    flareHi: { key: 'flare_hi_ft', vals: [20, 30, 40, 50, 60], label: function (v) { return String(v); }, unit: ' ft' },
    flareLo: { key: 'flare_lo_ft', vals: [0, 5, 10, 15, 20], label: function (v) { return String(v); }, unit: ' ft' },
    flareEnd: { key: 'flare_end_deg', vals: [-1, -0.5, 0, 0.5], label: function (v) { return (v > 0 ? '+' : '') + v.toFixed(1) + '°'; }, unit: '' },
    // terrain
    trLook: { key: 'terrain_look', vals: ['fill', 'sky', 'ridges'], label: word('tl_'), unit: '', str: true },
    trRes: { key: 'terrain_res', vals: [30, 90], label: function (v) { return L()['r_' + v]; }, unit: '', word: true },
    trCap: { key: 'terrain_cache_gb', vals: [1, 2, 4, 8, 16], label: function (v) { return String(v); }, unit: ' GB' },
    // camera source
    cam: { key: 'camera_source', vals: ['chaseplane', 'msfs'], label: word('cs_'), unit: '', str: true }
  };
  function word(prefix) { return function (v) { return L()[prefix + v] || v; }; }
  function swatch(v) {
    var name = L().c_custom;
    COLORS.forEach(function (c) { if (c[1] === v) name = L()[c[0]]; });
    return '<span class="sw" style="background:' + v + '"></span>' + name;
  }
  function pickVal(c, el) { var v = el.getAttribute('data-v'); return c.str ? v : parseFloat(v); }
  function picks() {
    Object.keys(PICKS).forEach(function (id) {
      var c = PICKS[id], cur = S.set[c.key], vals = c.vals.slice();
      if (vals.indexOf(cur) < 0 && typeof cur === 'number') { vals.push(cur); vals.sort(function (a, b) { return a - b; }); }  // e.g. set by hotkey
      if (id === 'color' && vals.indexOf(cur) < 0 && cur) vals.push(cur);  // a colour typed into settings.json
      var box = $('p-' + id);
      box.innerHTML = vals.map(function (v, i) {
        return '<span class="pick' + (v === cur ? ' sel' : '') + ((id === 'ladder' && !v) || c.str || c.word ? ' word' : '') + '" role="button" tabindex="0" data-v="' + v + '">' +
          c.label(v) + (i === vals.length - 1 ? c.unit : '') + '</span>';
      }).join('');
      var ps = box.querySelectorAll('.pick');
      each(ps, function (b) {
        press(b, function () {
          if (c.act) { act(c.act, pickVal(c, b)); S.set[c.key] = pickVal(c, b); touched[c.key] = 0; }
          else set(c.key, pickVal(c, b));
          each(ps, function (o) { cls(o, 'sel', o === b); });
          if (id === 'ladder' || c.key.indexOf('sty_') === 0 || c.key === 'hud_color') render();
          if (id === 'look') { applyLook(); words(); return; }
          summary(); fact();
        });
      });
    });
  }
  function summary() {  // the lines that say what a page is set to
    var s = S.set, l = L();
    $('n-fine').textContent = s.lead_ms + ' ms · ±' + s.flare_tol_deg.toFixed(1) + '° · ' + (s.ladder_step_deg ? s.ladder_step_deg + '°' : l.off);
    $('n-auto').textContent = (s.auto_show ? Math.round(s.auto_show_kt) + ' kt' : l.off) + ' · ' + (s.auto_hide ? Math.round(s.auto_hide_ft) + ' ft' : l.off);
    cls($('as-more'), 'idle', !s.auto_show);
    cls($('ah-more'), 'idle', !s.auto_hide);
    cls($('tdz-more'), 'idle', !s.show_tdz);
    $('n-hudlook').textContent = l['pre_' + s.hud_preset] || s.hud_preset;
    $('n-cam').textContent = l['cs_' + s.camera_source] || s.camera_source || '';
    $('n-anim').textContent = (l['a_' + s.anim_in] || '') + ' · ' + (l['a_' + s.anim_out] || '');
    $('n-appr').textContent = (s.show_tdz ? l['t_' + s.tdz_rule] : l.off) + ' · ' + Math.round(s.flare_hi_ft) + '/' + Math.round(s.flare_lo_ft) + ' ft';
    var moved = 0;
    LAYOUT.forEach(function (g) { g[1].forEach(function (r) { if (Math.abs(s[r[0]] - (r.length > 2 ? 1 : 0)) > 1e-6) moved++; }); });
    $('n-layout').textContent = moved ? '' + moved : '—';
    var pp = $('p-preset').querySelectorAll('.pick');
    each(pp, function (b) { cls(b, 'sel', b.getAttribute('data-v') === s.hud_preset); });
    var n = 0;
    ALL.forEach(function (g) { if (s[g[1]]) n++; });
    $('n-sym').textContent = fmt(l.symCount, { n: n, all: ALL.length });
    $('n-terrain').textContent = s.terrain_on ? (l['tl_' + s.terrain_look] || '') : l.off;
    cls($('tr-more'), 'idle', !s.terrain_on);
  }

  // ---------------------------------------------------------------- the voice
  var voiceKey = '', noticeUntil = 0, noticeTimer = null;
  function voiceFor() {
    if (!S.online) return 'offline';
    var w = S.st ? S.st.why : 'starting';
    return VOICE_TOOL[w] ? w : 'wait';
  }
  function factText(key) {
    var v = VOICES()[key][S.lang], hk = (S.set.hotkeys || {}).toggle || '';
    var f = v[2] && !hk ? v[2] : v[1], rw = S.lang === 'zh' ? '还没对准跑道' : 'no runway lined up';
    if (S.st && S.st.runway) rw = S.st.runway.icao + ' ' + S.st.runway.name + (S.lang === 'zh' ? ' 已对准' : ' lined up');
    return fmt(f, { rw: rw, g: S.set.glide_deg.toFixed(1), kt: Math.round(S.set.auto_show_kt), ft: Math.round(S.set.auto_hide_ft), hk: hk });
  }
  // the voice is said in two places: the home page and the peek card
  var VOICE_EL = ['voice', 'pk-voice'], FACT_EL = ['fact', 'pk-fact'];
  function fact() {
    if (Date.now() <= noticeUntil) return;
    var t = factText(voiceKey ? voiceKey.split('|')[0] : voiceFor());
    FACT_EL.forEach(function (id) { $(id).textContent = t; cls($(id), 'notice', false); });
  }
  function voice(quiet) {
    var key = voiceFor(), said = key + '|' + look();
    var put = function () { VOICE_EL.forEach(function (id) { $(id).textContent = VOICES()[key][S.lang][0]; }); fact(); };
    var swap = function (on) { VOICE_EL.concat(FACT_EL).forEach(function (id) { cls($(id), 'swap', on); }); };
    if (quiet || said === voiceKey || !voiceKey) { voiceKey = said; put(); return; }
    voiceKey = said;
    swap(true);
    setTimeout(function () { put(); swap(false); }, 520);
  }
  function notice(text) {  // the HUD's flash message, said here too for a moment
    noticeUntil = Date.now() + 2600;
    FACT_EL.forEach(function (id) { $(id).textContent = text; cls($(id), 'notice', true); });
    clearTimeout(noticeTimer);
    noticeTimer = setTimeout(function () { noticeUntil = 0; voice(true); }, 2700);
  }

  // the wordmark: the bird sits where the dot of the i would be (the i is dotless; measured once the font is in)
  function dotTheI() {
    try {
      var b = $('wm-i').getBBox();
      if (b.width > 0) $('wm-dot').setAttribute('transform', 'translate(' + (b.x + b.width / 2 + 2.5).toFixed(1) + ' ' + (b.y + 9).toFixed(1) + ')');
    } catch (e) { /* keep the drawn position */ }
  }

  // ---------------------------------------------------------------- everything that follows the state
  // the Camera source page: what the chosen source gives right now (status.camera, app.cpp camera_status)
  function camPaint(c, l) {
    var msfs = c.source === 'msfs';
    var line = !S.online ? '' : c.state === 'replay' ? l.camReplay : c.state === 'nosim' ? l.camNoSim
      : c.state === 'ok' ? l.camOk : c.state === 'outside' ? l.camOutside : msfs ? l.camNone : l.camNoCp;
    $('cam-state').textContent = line;
    var note = [];
    if (c.vfov != null && (c.state === 'ok' || c.state === 'outside')) note.push(fmt(l.camFov, { v: c.vfov.toFixed(1) }));
    if (msfs && c.state === 'none') {
      if (c.availability === 3) note.push(l.camUserOff);  // SIMCONNECT_CAMERA_USER_DISABLED
      if (c.error) note.push(fmt(l.camErr, { e: c.error })); else note.push(l.camWait);
    }
    $('cam-note').textContent = note.join(' · ');
    var o = msfs && S.online ? (!c.aircraft ? l.camOriginNoAc : c.origin == null ? fmt(l.camOriginNeed, { ac: c.aircraft })
      : fmt(l.camOrigin, { ac: c.aircraft, v: (c.origin > 0 ? '+' : '') + c.origin.toFixed(2) })) : '';
    $('cam-origin').textContent = o;
    $('cam-redo').style.display = msfs && c.origin != null ? '' : 'none';
  }

  function paint() {
    var s = S.set, st = S.st || {}, l = L();
    cls($('birdie'), 'offline', !S.online);
    cls($('birdie'), 'on', S.online && s.visible);
    $('power').setAttribute('aria-pressed', s.visible ? 'true' : 'false');
    each(document.querySelectorAll('.g'), function (el) {
      var g = glyph(el.getAttribute('data-k')), on = !!s[g[1]];
      cls(el, 'sel', on); cls(el, 'unsel', !on);
    });
    Object.keys(SCALES).forEach(paintScale);
    Object.keys(PICKS).forEach(function (id) {
      var ps = $('p-' + id).querySelectorAll('.pick'), cur = S.set[PICKS[id].key], found = false;
      each(ps, function (p) { var on = pickVal(PICKS[id], p) === cur; found = found || on; cls(p, 'sel', on); });
      if (!found) picks();
    });
    summary();
    cls($('lang-zh'), 'sel', S.lang === 'zh');
    cls($('lang-en'), 'sel', S.lang === 'en');
    // RA calibration for the aircraft Birdie sees now
    $('ra-ac').textContent = st.aircraft ? ' · ' + st.aircraft : '';
    var when = st.ra && /^\d{4}-\d\d-\d\d \d\d:\d\d/.exec(st.ra.note || '');  // the note is in the language of its day
    $('ra-note').innerHTML = !st.aircraft ? l.raNoAc : !st.ra ? l.raNone
      : st.ra.ft ? fmt(l.raFmt, { ft: '<span class="v num">' + st.ra.ft.toFixed(1) + '</span>' }) + (when ? ' \u00b7 ' + when[0] : '') : (st.ra.note || '');
    $('ra-btn').textContent = st.on_ground ? l.raBtn : l.raBtnWait;
    cls($('ra-btn'), 'dim', !st.on_ground);
    $('ra-clear').style.display = st.ra && st.ra.ft ? '' : 'none';
    camPaint(st.camera || {}, l);
    cls($('auto'), 'sel', !!st.autostart);
    $('auto').setAttribute('aria-pressed', st.autostart ? 'true' : 'false');
    each([['xw', 'xwind_mode'], ['as', 'auto_show'], ['ah', 'auto_hide'], ['accel', 'show_accel'], ['tdz', 'show_tdz'],
          ['tr-on', 'terrain_on'], ['tr-stream', 'terrain_stream'], ['tr-notes', 'terrain_notes']], function (t) {
      cls($(t[0]), 'sel', !!s[t[1]]);
      $(t[0]).setAttribute('aria-pressed', s[t[1]] ? 'true' : 'false');
    });
    $('ver').textContent = st.version ? 'Birdie ' + st.version : '';
    paintKeys();
    paintBorder();
    paintIngame();
    paintTerrain();
    paintWelcome();
    render();
    voice();
    preview();
    fitPanel();
  }

  // ---------------------------------------------------------------- HUD border (docs/HUD_BORDER.md)
  var clearAsked = 0;
  function paintBorder() {
    var l = L(), st = S.st || {}, b = st.border || null, state = b ? b.state : 'none';
    $('bd-who').textContent = !st.aircraft ? l.raNoAc : st.aircraft + ' · ' + (b && b.seat === 'R' ? l.seatR : l.seatL);
    $('bd-state').textContent = state === 'drawn' ? l.bdDrawn : state === 'mirrored' ? l.bdMirrored : l.bdNone;
    $('n-border').textContent = state === 'drawn' ? l.bdShortDrawn : state === 'mirrored' ? l.bdShortMirrored : l.bdShortNone;
    var can = !!(b && b.can_edit), editing = !!(b && b.editing);
    $('bd-edit').textContent = editing ? l.bdEditing : can ? l.bdEdit : l.bdEditNo;
    cls($('bd-edit'), 'dim', !can || editing);
    $('bd-eye').textContent = l.bdEye;
    $('bd-eye').style.display = b && b.eye_moved ? '' : 'none';
    $('bd-cut').style.display = b && b.cuts_fixed ? '' : 'none';
    $('bd-cut-t').textContent = l.bdCut;
    var fit = b && b.fit_scale;
    $('bd-fit').textContent = fit ? fmt(l.bdFit, { n: Math.round(fit * 100) }) : l.bdNoFit;
    cls($('bd-fit'), 'dim', !fit);
    $('bd-mirror').textContent = l.bdMirror;
    $('bd-mirror').style.display = b && b.other_own ? '' : 'none';
    if (Date.now() - clearAsked > 3500) { $('bd-clear').textContent = l.bdClear; cls($('bd-clear'), 'ask', false); }
    $('bd-clear').style.display = state !== 'none' ? '' : 'none';
  }

  // ---------------------------------------------------------------- in-sim panel (src/app/ingame.h)
  var igAsked = 0, welcomeOff = false;
  function paintIngame() {
    var l = L(), st = S.st || {}, ig = st.ingame || { state: 'nofolder' }, s = ig.state, note;
    $('t-ig').textContent = l.ig;
    $('ig-state').textContent = ' · ' + ({ none: l.igNone, current: l.igCurrent, old: l.igOld, foreign: l.igForeign, nofolder: l.igNoFolder }[s] || '');
    if (COUI) note = l.igInSim;
    else if (ig.note === 'failed') note = fmt(l.igFailed, { err: ig.err || '?' });
    else if (ig.note === 'removed' && s === 'none') note = l.igRemoved;
    else if (ig.note === 'exported') note = l.igExported;
    else if ((ig.note === 'installed' || ig.note === 'updated') && s === 'current' && ig.sim_running) note = l.igRestart;
    else note = s === 'none' ? l.igDescNone : s === 'current' ? fmt(l.igDescCurrent, { dir: ig.dir })
      : s === 'old' ? (ig.err ? fmt(l.igDescOld, { err: ig.err }) : l.igDescOldPlain)
      : s === 'foreign' ? fmt(l.igDescForeign, { dir: ig.dir }) : l.igDescNoFolder;
    $('ig-note').textContent = note;
    $('ig-main').textContent = s === 'old' ? l.igUpdate : l.igInstall;
    $('ig-main').style.display = !COUI && (s === 'none' || s === 'old') ? '' : 'none';
    $('ig-open').textContent = l.igOpen;
    $('ig-open').style.display = !COUI && s !== 'nofolder' ? '' : 'none';
    if (Date.now() - igAsked > 3500) { $('ig-remove').textContent = l.igRemove; cls($('ig-remove'), 'ask', false); }
    $('ig-remove').style.display = !COUI && (s === 'current' || s === 'old') ? '' : 'none';
    $('ig-pick').textContent = ig.chosen ? l.igAuto : l.igPick;
    $('ig-export').textContent = l.igExport;
    $('ig-more').style.display = COUI ? 'none' : '';
  }

  // ---------------------------------------------------------------- terrain (src/terrain): what it draws, and the data on disk
  var trAsked = {};  // button id -> when it asked "once more?"
  function bytes(b) { return b >= 1073741824 ? (b / 1073741824).toFixed(1) + ' GB' : Math.max(1, Math.round(b / 1048576)) + ' MB'; }
  function rate(r) { return r > 0 ? '· ' + (r >= 1048576 ? (r / 1048576).toFixed(1) + ' MB/s' : Math.round(r / 1024) + ' KB/s') : ''; }
  function errText(e) { var l = L(); return !e ? '' : e === 'network' ? l.e_network : e === 'disk_full' ? l.e_disk_full : fmt(l.e_other, { e: e }); }
  function twoStep(el, ask, fn) {  // the first click asks, a second one within 3.5 s does it
    var id = el.id || el.getAttribute('data-ask');
    if (Date.now() - (trAsked[id] || 0) < 3500) { trAsked[id] = 0; fn(); paintTerrain(); return; }
    trAsked[id] = Date.now();
    el.textContent = ask; cls(el, 'ask', true);
    setTimeout(paintTerrain, 3600);
  }
  function asking(id) { return Date.now() - (trAsked[id] || 0) < 3500; }
  function paintTerrain() {
    var l = L(), s = S.set, st = S.st || {}, t = st.terrain || null;
    var job = (t && t.job) || {}, streaming = job.kind === 'stream';
    $('tr-intro').textContent = l.trIntro;
    // streaming: what it does now, in words
    var sn = !t ? '' : streaming ? fmt(l.trsBusy, { tile: job.tile, i: Math.min(job.done + 1, job.total), n: job.total, rate: rate(job.rate) })
      : t.stream === 'hud_off' ? l.trsHudOff : t.stream === 'no_sim' ? l.trsNoSim : t.stream === 'manual' ? l.trsManual : l.trsOn;
    if (t && job.error && !streaming && s.terrain_stream) sn += ' ' + errText(job.error);
    $('tr-stream-note').textContent = sn;
    cls($('tr-stream-note'), 'busy', streaming);
    // disk
    if (t) {
      var lines = [];
      if (!t.cache_tiles && !t.pkg_tiles) lines.push(l.trUseNone);
      else {
        lines.push(fmt(l.trUseCache, { n: t.cache_tiles, size: bytes(t.cache_bytes), cap: s.terrain_cache_gb + ' GB' }));
        if (t.pkg_tiles) lines.push(fmt(l.trUsePkg, { n: t.pkg_tiles, size: bytes(t.pkg_bytes) }));
      }
      if (t.disk_free) lines.push(fmt(l.trUseFree, { free: bytes(t.disk_free) }));
      if (t.moving) lines.push(fmt(l.trMoving, { done: job.done || 0, total: job.total || 0 }));
      $('tr-use').innerHTML = lines.map(function (x) { return '<div>' + x + '</div>'; }).join('');
      $('tr-path').textContent = t.dir;
    }
    $('tr-dir-acts').style.display = COUI ? 'none' : '';
    $('tr-open').textContent = l.trOpen;
    $('tr-move').textContent = l.trMove;
    $('tr-move-def').textContent = l.trMoveDef;
    $('tr-move-def').style.display = t && !t.is_default ? '' : 'none';
    if (!asking('tr-clear')) { $('tr-clear').textContent = l.trClear; cls($('tr-clear'), 'ask', false); }
    if (!asking('tr-del-all')) { $('tr-del-all').textContent = l.trDelAll; cls($('tr-del-all'), 'ask', false); }
    $('tr-clear').style.display = t && t.cache_tiles ? '' : 'none';
    $('tr-del-all').style.display = t && t.pkg_tiles ? '' : 'none';
    $('tr-disk-note').textContent = COUI ? l.trDiskNote + ' ' + l.trInSim : l.trDiskNote;
    // packages
    $('tr-pkg-desc').textContent = l.trPkgDesc;
    $('tr-job').style.display = job.kind === 'package' ? '' : 'none';
    var lbl = job.label;
    ((t && t.regions) || []).forEach(function (r) { if (r.id === job.label) lbl = r.name; });
    $('tr-job-t').textContent = job.kind === 'package' ? fmt(l.trJob, { label: lbl, done: job.done, total: job.total, rate: rate(job.rate) }) +
      (job.failed ? fmt(l.trJobFail, { n: job.failed }) : '') + (job.error ? ' · ' + errText(job.error) : '') : '';
    $('tr-stop').textContent = l.trStop;
    var html = '';
    ((t && t.regions) || []).forEach(function (r) {
      var full = r.have >= r.tiles && r.tiles > 0, delId = 'trdel-' + r.id;
      html += '<div class="tr-rg"><span class="nm">' + r.name + '</span><span class="sz num">' +
        fmt(l.trTiles, { have: r.have, n: r.tiles, size: bytes(r.bytes) }) + '</span><span class="acts">' +
        (full ? '<span class="have">' + l.trHave + '</span>' : '<span class="act quiet" role="button" tabindex="0" data-get="' + r.id + '">' + l.trGet + '</span>') +
        (r.have ? '<span class="act plain' + (asking(delId) ? ' ask' : '') + '" role="button" tabindex="0" data-del="' + r.id + '" data-ask="' + delId + '">' +
          (asking(delId) ? l.trDelAsk : l.trDel) + '</span>' : '') + '</span></div>';
    });
    if ($('tr-regions').getAttribute('data-h') !== html) {  // rebuilt only when it changed (keeps the hover)
      $('tr-regions').setAttribute('data-h', html);
      $('tr-regions').innerHTML = html;
      each($('tr-regions').querySelectorAll('[data-get]'), function (b) { press(b, function () { act2('terrain_install', { region: b.getAttribute('data-get') }); }); });
      each($('tr-regions').querySelectorAll('[data-del]'), function (b) {
        press(b, function () { twoStep(b, L().trDelAsk, function () { act2('terrain_delete', { region: b.getAttribute('data-del') }); }); });
      });
    }
    // import
    $('tr-imp').style.display = COUI ? 'none' : '';
    $('tr-imp-desc').textContent = job.kind === 'import' ? fmt(l.trImporting, { done: job.done, total: job.total })
      : l.trImpDesc + (t && t.import_files ? ' ' + fmt(l.trImpHas, { n: t.import_files }) : '');
    $('tr-open-import').textContent = l.trOpenImp;
    $('tr-import').textContent = l.trImport;
    cls($('tr-import'), 'dim', !(t && t.import_files));
    $('tr-credit').textContent = l.trCredit;
    // the terrain's own border
    var bd = (st.border && st.border.terrain) || { state: 'none' }, can = !!(st.border && st.border.can_edit), ed = !!bd.editing;
    $('t-trBorder').textContent = l.trBorder;
    $('tr-bd-state').textContent = bd.state === 'drawn' ? l.tbDrawn : bd.state === 'mirrored' ? l.tbMirrored : l.tbNone;
    $('tr-bd-edit').textContent = ed ? l.bdEditing : !s.terrain_on ? l.tbOff : can ? l.bdEdit : l.bdEditNo;
    cls($('tr-bd-edit'), 'dim', !can || ed || !s.terrain_on);
    $('tr-bd-mirror').textContent = l.bdMirror;
    $('tr-bd-mirror').style.display = bd.other_own ? '' : 'none';
    if (!asking('tr-bd-clear')) { $('tr-bd-clear').textContent = l.bdClear; cls($('tr-bd-clear'), 'ask', false); }
    $('tr-bd-clear').style.display = bd.state !== 'none' ? '' : 'none';
    $('tr-bd-note').textContent = l.tbNote;
  }
  function act2(name, extra) { var m = { type: 'action', name: name }; for (var k in extra) m[k] = extra[k]; send(m); }

  function paintWelcome() {
    var on = WELCOME && !welcomeOff;
    cls(document.documentElement, 'w-on', on);
    if (!on) return;
    var l = L(), st = S.st || {}, ig = st.ingame || { state: 'nofolder' }, can = ig.state === 'none' || ig.state === 'old';
    var key = (S.set.hotkeys && S.set.hotkeys.toggle) || l.hkNone;
    $('w-title').textContent = l.wTitle;
    $('w-cp').textContent = l.wCp;
    $('w-win').textContent = l.wWin;
    $('w-key').textContent = fmt(l.wKey, { key: key });
    $('w-tray').textContent = l.wTray;
    $('w-ig').textContent = ig.state === 'current' ? (ig.note === 'installed' ? l.wDone : '') : can ? l.wIgNone
      : ig.note === 'failed' ? fmt(l.igFailed, { err: ig.err || '?' }) : l.wIgNoFolder;
    $('w-install').textContent = l.wInstall;
    $('w-install').style.display = can ? '' : 'none';
    $('w-ok').textContent = l.wOk;
  }

  function words() {
    var l = L(), map = { 't-sym': 'sym', 't-sym2': 'sym', 't-fine': 'fine', 't-fine2': 'fine', 't-path': 'path', 't-world': 'world', 't-inst': 'inst',
      't-size': 'size', 't-bright': 'bright', 't-glide': 'glide', 't-lead': 'lead', 't-tol': 'tol', 't-ladder': 'ladder', 't-rah': 'rah', 't-ra': 'ra',
      't-xw': 'xw', 'ra-clear': 'raClear', 't-auto': 'auto', 't-log': 'log', 't-quit': 'quit', 't-pk': 'power',
      't-border': 'border', 't-border2': 'border', 'bd-desc': 'bdDesc', 't-look': 'look', 't-autonav': 'autoNav', 't-autonav2': 'autoNav',
      't-as': 'as', 't-ah': 'ah', 't-ahft': 'ahFt', 't-ahs': 'ahS', 'auto-desc': 'autoDesc',
      't-hudlook': 'hudLook', 't-hudlook2': 'hudLook', 't-preset': 'preset', 't-layout': 'layout', 't-layout2': 'layout',
      't-animnav': 'animNav', 't-animnav2': 'animNav', 't-elements': 'elements', 't-finish': 'finish', 't-fpv': 'fpv',
      't-spdS': 'spdS', 't-altS': 'altS', 't-vsS': 'vsS', 't-ladderS': 'ladderS', 't-hdgS': 'hdgS', 't-rollS': 'rollS',
      't-accel': 'accel', 't-color': 'color', 't-font': 'font', 't-outline': 'outline', 't-glow': 'glow',
      'lay-reset': 'layReset', 'lay-desc': 'layDesc', 't-animIn': 'animIn', 't-animOut': 'animOut', 't-animTime': 'animTime',
      'try-in': 'tryIn', 'try-out': 'tryOut', 'anim-desc': 'animDesc', 't-appr': 'appr', 't-appr2': 'appr', 't-tdz': 'tdz',
      't-tdzRule': 'tdzRule', 't-tdzStyle': 'tdzStyle', 't-flareG': 'flareG', 't-flareRa': 'flareRa', 't-flareHi': 'flareHi',
      't-flareLo': 'flareLo', 't-flareEnd': 'flareEnd', 'appr-desc': 'apprDesc', 't-keys': 'keys', 't-keys2': 'keys',
      'hk-reset': 'hkReset', 'hk-desc': 'hkDesc', 't-terrain': 'terrain', 't-terrain2': 'terrain', 't-trOn': 'trOn',
      't-trLook': 'trLook', 't-trStrength': 'trStrength', 't-trStream': 'trStream', 't-trRes': 'trRes', 't-trCap': 'trCap',
      't-trNotes': 'trNotes', 't-trDisk': 'trDisk', 't-trPkg': 'trPkg',
      't-camnav': 'camNav', 't-camnav2': 'camNav', 't-cam': 'cam', 't-camst': 'camSt', 'cam-desc': 'camDesc', 'cam-redo': 'camRedo' };
    layoutWords(); keysWords();
    for (var id in map) $(id).textContent = l[map[id]];
    document.documentElement.lang = S.lang === 'zh' ? 'zh-CN' : 'en';
    glyphs(); picks(); describe(null, true);
    voiceKey = ''; pvShown = null; paint();
  }

  // click only: no key works a button (11/17: in the sim a clicked button kept the focus, and Space then toggled the HUD)
  function press(el, fn) {
    el.addEventListener('click', function (e) { e.preventDefault(); fn(e); });
  }

  function controls() {
    press($('power'), function () { set('visible', !S.set.visible); paint(); });
    press($('pk-power'), function () { set('visible', !S.set.visible); paint(); });
    press($('xw'), function () { set('xwind_mode', !S.set.xwind_mode); paint(); });
    press($('as'), function () { set('auto_show', !S.set.auto_show); paint(); });
    press($('ah'), function () { set('auto_hide', !S.set.auto_hide); paint(); });
    press($('accel'), function () { set('show_accel', !S.set.show_accel); paint(); });
    press($('tdz'), function () { set('show_tdz', !S.set.show_tdz); paint(); });
    press($('tr-on'), function () { set('terrain_on', !S.set.terrain_on); paint(); });
    press($('tr-stream'), function () { set('terrain_stream', !S.set.terrain_stream); paint(); });
    press($('tr-notes'), function () { set('terrain_notes', !S.set.terrain_notes); paint(); });
    press($('tr-open'), function () { act('terrain_open'); });
    press($('tr-move'), function () { act('terrain_move'); });
    press($('tr-move-def'), function () { act('terrain_move_default'); });
    press($('tr-clear'), function () { twoStep($('tr-clear'), L().trClearAsk, function () { act('terrain_clear_cache'); }); });
    press($('tr-del-all'), function () { twoStep($('tr-del-all'), L().trDelAllAsk, function () { act2('terrain_delete', { region: 'all' }); }); });
    press($('tr-stop'), function () { act('terrain_stop'); });
    press($('tr-bd-edit'), function () {
      var b = S.st && S.st.border;
      if (S.set.terrain_on && b && b.can_edit && !(b.terrain && b.terrain.editing)) act('terrain_border_edit');
    });
    press($('tr-bd-mirror'), function () { act('terrain_border_mirror'); });
    press($('tr-bd-clear'), function () { twoStep($('tr-bd-clear'), L().bdClearAsk, function () { act('terrain_border_clear'); }); });
    press($('tr-open-import'), function () { act('terrain_open_import'); });
    press($('tr-import'), function () { act('terrain_import'); });
    press($('lay-reset'), function () { act('reset_layout'); });
    press($('try-in'), function () { act('anim_test', 'in'); });
    press($('try-out'), function () { act('anim_test', 'out'); });
    press($('hk-reset'), function () { listen(null); act('hotkeys_reset'); });
    setInterval(hold, 8000);
    // in the sim nothing in the panel keeps the keyboard focus after a click, so Space, arrows etc. stay with the sim
    if (COUI) document.addEventListener('click', function () {
      var a = document.activeElement;
      if (a && a !== document.body && a.blur) a.blur();
    }, true);
    press($('lang-zh'), function () { set('language', 'zh-CN'); S.lang = 'zh'; words(); });
    press($('lang-en'), function () { set('language', 'en'); S.lang = 'en'; words(); });
    press($('ra-btn'), function () { if (S.st && S.st.on_ground) act('ra_calibrate'); });
    press($('ra-clear'), function () { act('ra_clear'); });
    press($('cam-redo'), function () { act('cam_origin_redo'); });
    press($('auto'), function () { act('autostart', !(S.st && S.st.autostart)); });
    press($('t-log'), function () { act('open_log'); });
    press($('ig-main'), function () { act('ingame_install'); });
    press($('ig-open'), function () { act('ingame_open'); });
    press($('ig-remove'), function () {  // two steps, like clearing a border
      var el = $('ig-remove');
      if (Date.now() - igAsked < 3500) { igAsked = 0; act('ingame_remove'); return; }
      igAsked = Date.now();
      el.textContent = L().igRemoveAsk; cls(el, 'ask', true);
      setTimeout(function () { if (Date.now() - igAsked >= 3400) paintIngame(); }, 3500);
    });
    press($('ig-pick'), function () { act(S.st && S.st.ingame && S.st.ingame.chosen ? 'ingame_auto' : 'ingame_pick'); });
    press($('ig-export'), function () { act('ingame_export'); });
    press($('w-install'), function () { act('ingame_install'); });
    press($('w-ok'), function () { welcomeOff = true; paint(); });
    press($('bd-edit'), function () { var b = S.st && S.st.border; if (b && b.can_edit && !b.editing) act('border_edit'); });
    press($('bd-fit'), function () { var b = S.st && S.st.border; if (b && b.fit_scale) act('border_fit'); });
    press($('bd-mirror'), function () { act('border_mirror'); });
    press($('bd-clear'), function () {  // two gentle steps: a trace takes a minute to redo
      var el = $('bd-clear');
      if (Date.now() - clearAsked < 3500) { clearAsked = 0; act('border_clear'); return; }
      clearAsked = Date.now();
      el.textContent = L().bdClearAsk; cls(el, 'ask', true);
      setTimeout(function () { if (Date.now() - clearAsked >= 3400) paintBorder(); }, 3500);
    });
    each(document.querySelectorAll('[data-go]'), function (b) { press(b, function () { go(parseInt(b.getAttribute('data-go'), 10)); }); });
    $('panel').addEventListener('scroll', function () { if ($('panel').scrollLeft) $('panel').scrollLeft = 0; });  // Tab into a hidden page
    var asked = 0, quit = $('t-quit');
    press(quit, function () {  // two gentle steps: it is easy to mean "hide the HUD" here
      if (Date.now() - asked < 3500) { act('quit'); return; }
      asked = Date.now();
      quit.textContent = L().quitAsk; cls(quit, 'ask', true);
      setTimeout(function () { quit.textContent = L().quit; cls(quit, 'ask', false); }, 3500);
    });
    document.addEventListener('keydown', function (e) {
      if (onListenKey(e)) return;  // recording a hotkey: every key is for it
      if (COUI || e.keyCode !== 27) return;  // Esc: back, then away (Birdie's own window only; the sim keeps its keys)
      if (S.page === 10) go(0); else if (S.page >= 3) go(2); else if (S.page) go(0); else host({ host: 'close' });
    });
    if (PEEK) {  // a click anywhere but the switch opens the window; the strip lifts its line in each time it shows
      $('peekcard').addEventListener('click', function (e) {
        if (e.target.closest && e.target.closest('.knob')) return;
        host({ host: 'open' });
      });
      window.chrome.webview.addEventListener('message', function (e) {
        if (e.data !== 'show') return;
        fitHost();
        each(document.querySelectorAll('.peekcard .rise'), function (el) { cls(el, 'rise', false); void el.offsetWidth; cls(el, 'rise', true); });
      });
    } else if (HOST) {
      press($('x'), function () { focusOn(null); host({ host: 'close' }); });
      // fallback when the browser has no app-region support: the page asks the window to move
      var drag = function (e) {
        if (e.button !== 0 || (e.target.closest && e.target.closest('[role="button"]'))) return;
        host({ host: 'drag' });
      };
      $('grip').addEventListener('mousedown', drag);
      $('head').addEventListener('mousedown', drag);
      window.addEventListener('blur', function () { focusOn(null); });
    }
  }

  // the root size follows the panel's width (the sim lets the user resize it)
  var lastW = 0;
  function fit() {
    var w = COUI ? ($('birdie').clientWidth || window.innerWidth) : DESIGN_W;  // the window and the peek: 14 px
    if (w !== lastW && w > 0) {
      lastW = w;
      document.documentElement.style.fontSize = Math.max(11, Math.min(22, w / DESIGN_W * 14)).toFixed(2) + 'px';
      if (pvShown) $('pvwrap').style.height = $('pv').offsetHeight + 'px';
      fitPanel();
    }
  }

  var started = false;
  function start() {
    if (started) return;
    started = true;
    cls(document.documentElement, COUI ? 'coui' : HOST ? 'host' : 'web', true);
    if (PEEK) {
      cls(document.documentElement, 'peek', true);
      if (document.fonts && document.fonts.ready) document.fonts.ready.then(function () { dotTheI(); fitHost(); });
    }
    try { var kept = window.localStorage && localStorage.getItem('birdie.look'); if (STYLES.indexOf(kept) >= 0) S.set.ui_style = kept; } catch (e) { /* no storage */ }
    var q = /[?&]look=(\w+)/.exec(location.search);  // previews
    if (q && STYLES.indexOf(q[1]) >= 0) S.set.ui_style = q[1];
    applyLook();
    var pref = (navigator.language || '').toLowerCase();
    S.lang = pref.indexOf('zh') === 0 || !pref ? 'zh' : 'en';
    hud = $('hud');
    layoutPages(); buildLayout(); buildKeys();
    lights(); scales(); controls();
    words();
    go(location.hash === '#border' ? 3 : location.hash === '#terrain' ? 10 : 0);  // #border: back from tracing the border in Birdie's own window
    fit();
    window.addEventListener('resize', fit);
    if (COUI) setInterval(fit, 500);  // the sim resizes panels without a resize event
    window.addEventListener('load', fitPanel);
    setTimeout(fitPanel, 300);  // fonts arrive late
    connect();
  }

  // the sim's panel element (TemplateElement comes from /JS/common.js); a plain element in a browser
  var Base = window.TemplateElement || HTMLElement;
  class BirdiePanel extends Base {
    connectedCallback() {
      if (super.connectedCallback) super.connectedCallback();
      if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
      else setTimeout(start, 0);
    }
  }
  window.customElements.define('ingamepanel-birdie', BirdiePanel);
  if (typeof checkAutoload === 'function') checkAutoload();
})();
