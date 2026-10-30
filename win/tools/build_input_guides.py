#!/usr/bin/env python3
"""Build compact offline gesture guides and a licensed controller-model guide.

Only the Python standard library is needed. User reference photographs are
never read, embedded, copied or distributed by this generator.
"""
import base64
import html
import re
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
FINGER_MAP_SVG = '''<svg viewBox="0 0 330 440" role="img" aria-labelledby="palm-title palm-desc" xmlns="http://www.w3.org/2000/svg">
<title id="palm-title">Open palm with five numbered fingers</title>
<desc id="palm-desc">One: thumb. Two: index. Three: middle. Four: ring. Five: pinky. Their controller actions are listed beside the palm.</desc>
<defs><linearGradient id="palm-fill" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#314c68"/><stop offset="1" stop-color="#20334b"/></linearGradient></defs>
<path d="M128 413 L116 353 C102 336 91 314 80 291 L31 227 C21 214 24 200 35 196 C46 192 56 199 64 209 L103 252 L95 115 C94 99 103 87 116 87 C130 87 138 98 139 113 L143 226 L143 62 C143 45 152 34 166 34 C181 34 188 46 187 64 L184 225 L193 88 C194 72 204 63 217 65 C231 67 236 78 235 94 L226 232 L240 137 C242 122 252 116 263 119 C276 122 281 133 279 147 L275 279 C275 310 269 334 255 357 L245 413 Z" fill="url(#palm-fill)" stroke="#8ca8c9" stroke-width="3.5" stroke-linejoin="round"/>
<g fill="none" stroke="#617e9f" stroke-width="2.5" stroke-linecap="round"><path d="M118 283 C147 247 193 254 233 279"/><path d="M113 296 C125 321 144 340 169 352"/><path d="M149 377 C175 383 203 382 227 373"/><path d="M139 228 L145 257 M184 226 L183 254 M226 233 L219 263"/></g>
<g font-family="system-ui,sans-serif" font-size="17" font-weight="700" text-anchor="middle" fill="#0c1320">
<circle cx="52" cy="219" r="17" fill="#f4c56b"/><text x="52" y="225">1</text>
<circle cx="118" cy="121" r="17" fill="#61d8ed"/><text x="118" y="127">2</text>
<circle cx="165" cy="73" r="17" fill="#c4a1ff"/><text x="165" y="79">3</text>
<circle cx="213" cy="111" r="17" fill="#7ce2b5"/><text x="213" y="117">4</text>
<circle cx="258" cy="157" r="17" fill="#c2cddb"/><text x="258" y="163">5</text>
</g></svg>'''

COMPACT_STYLE = '''
body{font:16px/1.6 system-ui,sans-serif;max-width:850px;margin:32px auto;padding:0 22px;background:#101b2a;color:#e8eff8}
*{box-sizing:border-box}a{color:#8acaff}p{line-height:1.65}h1,h2{line-height:1.25}strong{color:white}nav,footer{font-size:14px}nav{display:flex;gap:14px;flex-wrap:wrap}footer{margin:32px 0}
table{width:100%;border-collapse:collapse;margin:18px 0}th,td{text-align:left;vertical-align:top;padding:12px;border-bottom:1px solid #34455e}th{background:#18283b;color:white}th:first-child,td:first-child{width:30%}
.hand-map{display:grid;grid-template-columns:minmax(180px,270px) 1fr;gap:30px;align-items:center;padding:22px;background:#18283b;border:1px solid #34455e;border-radius:16px;margin:24px 0 8px}
.hand-map svg{width:100%;max-height:320px}.hand-map dl{margin:0}.hand-map dt{font-weight:700;color:white;margin:12px 0}.marker{display:inline-block;width:25px;height:25px;border-radius:50%;text-align:center;color:#0c1320;margin-right:8px}.hand-note{font-size:14px;color:#b9c9de}
.tabs{display:flex;gap:8px;flex-wrap:wrap;margin:24px 0}button[role=tab]{padding:10px 14px;border:1px solid #486079;border-radius:8px;background:#18283b;color:#e8eff8;font:inherit;cursor:pointer}button[aria-selected=true]{background:#68dfca;color:#0c1320;border-color:#68dfca}button:focus-visible,a:focus-visible{outline:3px solid #f4c56b;outline-offset:3px}
p{overflow-wrap:anywhere}.touchpad-art{max-width:520px;margin:20px auto;background:#18283b;border:1px solid #34455e;border-radius:14px;padding:12px}.touchpad-art svg{width:100%;height:auto}.touchpad-art figcaption{font-size:14px;color:#b9c9de;text-align:center}[hidden]{display:none!important}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#18283b;padding:14px;border-radius:8px}
@media(max-width:620px){.hand-map{grid-template-columns:1fr;gap:8px}.hand-map svg{max-height:260px}.hand-map dl{display:flex;flex-wrap:wrap;gap:4px 16px}.hand-map dt{margin:0}.tabs button{width:100%;text-align:left}th,td{padding:9px 7px}}
@media print{body{background:white;color:black}strong{color:black}nav,.tabs{display:none}[role=tabpanel][hidden]{display:block!important}th{color:black;background:#eee}}
'''

STYLE='''
:root{color-scheme:dark;font-family:system-ui,-apple-system,"Segoe UI",sans-serif;color:#e9f1fc;background:#0b1220}
*{box-sizing:border-box}body{margin:0}a{color:#8bc7ff}nav,main{max-width:1200px;margin:auto;padding:24px}nav{display:flex;gap:16px;flex-wrap:wrap;padding-bottom:0;font-size:14px}
h1{font-size:clamp(28px,4vw,42px);line-height:1.1;margin:26px 0 14px}h2{font-size:24px;margin:32px 0 14px}h3{font-size:19px;margin:0 0 8px}p{line-height:1.65;margin:8px 0 15px;color:#bccde1}.intro{max-width:900px}
.tabs{display:flex;gap:8px;flex-wrap:wrap;border-bottom:1px solid #32445e;padding-bottom:14px;margin:26px 0}button[role=tab]{border:1px solid #405370;border-radius:10px;background:#18263d;color:#d7e4f5;padding:13px 18px;font:600 15px inherit;cursor:pointer}button[aria-selected=true]{background:#64dfce;color:#102520;border-color:#64dfce}button:focus-visible,a:focus-visible{outline:3px solid #91c9ff;outline-offset:4px}[hidden]{display:none!important}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px}.card{background:#142137;border:1px solid #2a405c;border-radius:18px;overflow:hidden}.card .art{background:linear-gradient(145deg,#192a43,#101b2d);padding:12px;overflow:hidden}.card svg{display:block;width:100%;height:auto;max-height:360px}.card .copy{padding:20px 22px}.card p{margin-bottom:0}.tag{color:#64dfce;font-size:13px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;display:block;margin-bottom:7px}.wide{grid-column:1/-1}.wide .art svg{max-height:360px}.notice{border-left:3px solid #64dfce;padding:10px 18px;margin:20px 0;background:#132b32;border-radius:0 10px 10px 0}.thumb-grid{grid-template-columns:repeat(3,minmax(0,1fr))}.thumb-grid .card svg{max-height:240px}.mapping svg{max-height:none}footer{border-top:1px solid #293d56;margin-top:34px;padding-top:20px;font-size:13px;color:#91a7c3}
@media(max-width:700px){nav,main{padding:18px}.grid,.thumb-grid{grid-template-columns:1fr}.card .copy{padding:17px}.tabs button{width:100%;text-align:left}.wide{grid-column:auto}}
@media print{body{background:white;color:black}nav,.tabs{display:none}[role=tabpanel][hidden]{display:block!important}.card{break-inside:avoid}p{color:#222}}
'''

TABS='''
<script>
(()=>{const tabs=[...document.querySelectorAll('[role=tab]')];
function select(id,focus=false){const found=tabs.find(t=>t.dataset.panel===id)||tabs[0];for(const t of tabs){const on=t===found;t.setAttribute('aria-selected',String(on));t.tabIndex=on?0:-1;document.getElementById(t.dataset.panel).hidden=!on}if(focus)found.focus();return found.dataset.panel}
for(const t of tabs){t.addEventListener('click',()=>{const id=select(t.dataset.panel);history.replaceState(null,'','#'+id)});t.addEventListener('keydown',e=>{let i=tabs.indexOf(t);if(e.key==='ArrowRight')i=(i+1)%tabs.length;else if(e.key==='ArrowLeft')i=(i+tabs.length-1)%tabs.length;else if(e.key==='Home')i=0;else if(e.key==='End')i=tabs.length-1;else return;e.preventDefault();const id=select(tabs[i].dataset.panel,true);history.replaceState(null,'','#'+id)})}
addEventListener('hashchange',()=>select(location.hash.slice(1)));select(location.hash.slice(1));})();
</script>'''


def page(title,contents,ru=False,support=False,style=None):
    suffix='.ru' if ru else ''
    other='' if ru else '.ru'
    filename='SUPPORT-VR-CONTROLLERS' if support else 'GESTURES'
    return f'''<!doctype html><html lang="{'ru' if ru else 'en'}"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>{title} — XR Gate</title><style>{style or STYLE}</style></head><body>
<nav aria-label="{'Справка' if ru else 'Help'}"><a href="START-HERE{suffix}.html">XR Gate</a><a href="GESTURES{suffix}.html">Gestures</a><a href="SUPPORT-VR-CONTROLLERS{suffix}.html">Support VR controllers</a><a href="{filename}{other}.html" lang="{'en' if ru else 'ru'}">{'English' if ru else 'Русский'}</a></nav>
<main><h1>{title}</h1>{contents}<footer>XR Gate · <a href="https://github.com/vladoshub/xr-gate">GitHub</a></footer></main>{TABS}</body></html>'''


def tabbar(entries):
    return '<div class="tabs" role="tablist" aria-label="Input guide">'+''.join(
        f'<button type="button" role="tab" id="tab-{id}" data-panel="{id}" aria-controls="{id}" aria-selected="{str(i==0).lower()}">{label}</button>'
        for i,(id,label) in enumerate(entries))+'</div>'


def panel(id,body):
    return f'<section role="tabpanel" id="{id}" aria-labelledby="tab-{id}">{body}</section>'


def gestures(ru=False):
    def tr(en,rus):return rus if ru else en
    def table(headers,rows):
        return '<table><thead><tr>'+''.join('<th scope="col">'+h+'</th>' for h in headers)+'</tr></thead><tbody>'+''.join(
            '<tr>'+''.join('<td>'+v+'</td>' for v in row)+'</tr>' for row in rows)+'</tbody></table>'
    entries=[('grab-pinch','Gestures (grab / pinch)'),('controller-emulation','Gestures (controller emulation)')]
    intro='<p>'+tr('Select the matching mode in the launcher’s Controllers list. Simple gestures are the default.',
        'Выбери соответствующий режим в списке Controllers лаунчера. По умолчанию используются простые жесты.')+'</p>'
    picture=FINGER_MAP_SVG
    if ru:
        picture=picture.replace('Open palm with five numbered fingers','Открытая ладонь с пятью пронумерованными пальцами').replace(
            'One: thumb. Two: index. Three: middle. Four: ring. Five: pinky. Their controller actions are listed beside the palm.',
            'Один — большой, два — указательный, три — средний, четыре — безымянный, пять — мизинец. Действия описаны в таблицах ниже.')
    finger_map='<section class="hand-map" aria-label="'+tr('Finger map','Схема пальцев')+'">'+picture+'<dl>'
    for number,color,en,rus in [(1,'#f4c56b','Thumb','Большой'),(2,'#61d8ed','Index','Указательный'),
            (3,'#c4a1ff','Middle','Средний'),(4,'#7ce2b5','Ring','Безымянный'),(5,'#c2cddb','Pinky','Мизинец')]:
        finger_map+=f'<dt><span class="marker" style="background:{color}">{number}</span>'+tr(en,rus)+'</dt>'
    finger_map+='</dl></section><p class="hand-note">'+tr(
        'This drawing identifies the fingers. The ready pose for controller emulation is described below.',
        'На рисунке обозначены пальцы. Нейтральная поза для эмуляции контроллеров описана ниже.')+'</p>'
    em='<h2>'+tr('Ready pose','Нейтральное положение')+'</h2><p>'+tr(
        'Keep your index and middle fingers straight, ring and pinky bent, and thumb relaxed near the palm. Turn your hands inward toward your body so the cameras can see them well. In VR, the hands/controllers are rotated forward by the configured offset.',
        'Указательный и средний прямые, безымянный и мизинец согнуты, большой расслаблен рядом с ладонью. Поверни кисти внутрь, к телу, для лучшего распознавания камерами. В VR руки и контроллеры довёрнуты вперёд настроенным смещением.')+'</p>'
    em+='<h2>'+tr('Buttons','Кнопки')+'</h2>'+table([tr('Button','Кнопка'),tr('Gesture','Жест')],[
        ('Trigger',tr('Bend your index finger; straighten to release.','Согни указательный; выпрями, чтобы отпустить.')),
        ('Grip',tr('Bend your middle finger; straighten to release.','Согни средний; выпрями, чтобы отпустить.')),
        ('X (left) / A (right)',tr('Make a thumb–index circle; keep the other three fingers straight.','Соедини большой и указательный в кольцо; остальные три пальца прямые.')),
        ('Y (left) / B (right)',tr('Make a thumb–index circle; bend the other three fingers.','Соедини большой и указательный в кольцо; остальные три пальца согнуты.')),
        (tr('Thumbstick click / touchpad press','Нажатие стика / тачпада'),tr('Open one palm toward yourself, with all five fingers straight and spread. Change the pose to release.',
            'Раскрой одну ладонь к себе: все пять пальцев прямые и разведены. Смени позу, чтобы отпустить.')),
        ('Menu',tr('Hold both hands open, palms away from you, like a stop sign. You should see the backs of your hands.',
            'Ненадолго удерживай обе раскрытые ладони от себя, как знак «стоп». Ты видишь тыльную сторону рук.'))])
    em+='<p><strong>'+tr('For Trigger and Grip, try bending the finger only partway. A full curl may not be necessary.',
        'Для Trigger и Grip можно попробовать сгибать палец не до конца.')+'</strong></p><p>'+tr(
        'Left hand: X/Y. Right hand: A/B. Trigger, Grip and D-pad can work together. The open-palm touchpad gesture pauses movement, Trigger and Grip.',
        'Левая рука — X/Y, правая — A/B. Trigger, Grip и D-pad могут работать вместе. Отдельный жест нажатия тачпада приостанавливает движение, Trigger и Grip.')+'</p>'
    em+='<h2>'+tr('Thumb movement','Движение большим пальцем')+'</h2><p>'+tr('Keep your wrist still and your thumb away from your palm.',
        'Держи кисть неподвижно, а большой — отведённым от ладони.')+'</p>'+table([tr('Movement','Движение'),tr('Thumb gesture','Жест большого пальца')],[
        (tr('Neutral','Нейтраль'),tr('Keep the thumb relaxed and slightly away from the palm; no movement.','Большой расслаблен и немного отведён; движения нет.')),
        (tr('Forward','Вперёд'),tr('Straighten and slightly arch the thumb away from the palm.','Выпрями и слегка выгни большой от ладони.')),
        (tr('Backward','Назад'),tr('Clearly curl the tip toward the palm while keeping the base away.','Отчётливо согни кончик к ладони, оставив основание отведённым.')),
        (tr('Left / right','Влево / вправо'),tr('Move the thumb in the desired direction.','Отведи большой в нужную сторону.')),
        (tr('Diagonal','Диагональ'),tr('Combine sideways movement with bending or straightening.','Совмести боковое движение со сгибанием или разгибанием.')),
        (tr('D-pad off','D-pad выключен'),tr('Press the thumb against the palm.','Плотно прижми большой к ладони.'))])
    em+='<p>'+tr('If wrist D-pad is selected in your profile, keep your thumb out and tilt the wrist in the desired direction. Diagonals work too. Tuck the thumb to stop. Wrist and thumb modes are alternatives.',
        'Если в профиле выбран D-pad от кисти, отведи большой и наклоняй кисть в нужную сторону. Диагонали тоже работают. Прижми большой для остановки. Режимы кисти и большого пальца выбираются по отдельности.')+'</p>'
    simple='<h2>'+tr('Simple gestures','Простые жесты')+'</h2><p>'+tr(
        'No calibration is needed. Pinch and palm Grip also work during an active touchpad session; V clicks the panel instead of X/A.',
        'Калибровка не требуется. Щипок и ладонь к себе (Grip) работают также во время активного тачпада; V нажимает панель вместо X/A.')+'</p>'+table([tr('Button','Кнопка'),tr('Gesture','Жест')],[
        ('Trigger',tr('Pinch your thumb and index fingertips together. Separate them to release.','Соедини кончики большого и указательного в щипок. Разведи, чтобы отпустить.')),
        ('Grip',tr('Open your palm toward yourself, with all five fingers straight and spread.','Раскрой ладонь к себе: все пять пальцев прямые и разведены.')),
        ('Grip + Trigger',tr('Close your hand into a fist, with the thumb folded against the fingers. This works outside an active touchpad.',
            'Сожми кулак, прижав большой к согнутым пальцам. Работает вне активного тачпада.')),
        ('X (left) / A (right)',tr('Outside the touchpad, show V: spread your straight index and middle fingers; bend ring and pinky. Fingers point up, palm away from you. An angled wrist does not press the button.','Вне тачпада покажи V: прямые указательный и средний разведены, безымянный и мизинец согнуты. Пальцы вверх, ладонь от себя. При наклонённой кисти кнопка не нажимается.')),
        ('Y (left) / B (right)',tr('Curl index, middle, ring and pinky into a fist. Keep the thumb away from the palm and point its main phalanx straight up. Its tip may be straight or bent; no backward arch is required.','Сожми указательный, средний, безымянный и мизинец в кулак. Большой отведи от ладони и направь прямо вверх. Кончик может быть прямым или согнутым, обязательного загиба назад нет.')),
        ('Menu',tr('Straighten only the index finger on each hand. Point the two index fingers toward each other, with both hands visible.',
            'Выпрями только указательные на обеих руках. Направь их друг на друга, удерживая обе руки в поле зрения.'))])
    simple+='<h2>'+tr('Virtual touchpad','Виртуальный тачпад')+'</h2><p>'+tr(
        'Enabled by default, independently for each hand. Fully straighten all five fingers and separate thumb and index. Point the open palm away from your head, like a stop sign: you see the back of your hand, with the fingers unfolded at the knuckles. Keep this pose briefly to activate. Its position relative to your head then becomes the centre. Head movement and rotation are accounted for continuously.',
        'Включён по умолчанию, отдельно для каждой руки. Полностью раскрой все пять пальцев и разведи большой с указательным. Направь ладонь от головы, как знак «стоп»: ты видишь тыльную сторону руки, пальцы выпрямлены и в костяшках. Ненадолго сохрани позу для включения. Тогда положение руки относительно головы станет центром. Перемещение и повороты головы учитываются постоянно.')+'</p>'+touchpad_picture(ru)+table([tr('Action','Действие'),tr('Gesture','Жест')],[
        (tr('Move on the panel','Движение по панели'),tr('Move the hand left/right or up/down relative to your head. Diagonals work. The edge is 10 cm from the centre; movement beyond it stays at the edge.',
            'Двигай руку влево/вправо и вверх/вниз относительно головы. Диагонали работают. Край находится в 10 см от центра; за ним значение остаётся на краю.')),
        (tr('Press','Нажать'),tr('After activating the panel, show V: spread your straight index and middle fingers; bend ring and pinky. Point the fingers up, palm away from you. Keep moving the hand to change direction while holding V. An angled wrist releases the click while the panel stays active. Release V to release the click. A fist does not click.',
            'После включения панели покажи V: прямые указательный и средний разведены, безымянный и мизинец согнуты. Пальцы направлены вверх, ладонь от себя. Двигай руку с V, чтобы менять направление при удержании нажатия. Наклон кисти отпускает клик, сама панель остаётся активной. Отпусти V, чтобы отпустить клик. Кулак не нажимает панель.')),
        (tr('Release','Отпустить'),tr('Open the palm away again. The centre stays in place.','Снова раскрой ладонь от себя. Центр останется прежним.')),
        (tr('Exit / set a new centre','Выйти / задать новый центр'),tr('Make a fist with the other four fingers, lift the thumb away from the palm and point it straight up. Its tip may be straight or bent. An open hand, V or tucked thumb does not exit. This exits without pressing B/Y; release the pose before using it as a normal button. Loss of hand or head tracking also exits. Wrist rotation does not exit, even past 35 degrees. The next activation captures a new centre.',
            'Сожми четыре пальца в кулак, большой отведи от ладони и направь прямо вверх. Кончик может быть прямым или согнутым. Открытая ладонь, V и прижатый большой не выключают панель. Это выход без нажатия B/Y; отпусти позу перед её использованием как обычной кнопки. Потеря трекинга руки или головы тоже выключает панель. Вращение кисти, в том числе за 35°, не выключает её. Следующее включение задаст новый центр.'))])
    simple+='<p>'+tr('While the panel is active, pinch sends Trigger, an open palm toward yourself sends Grip, and V clicks the panel. These gestures keep the panel axes working. V sends no X/A during the session; no replacement gesture is assigned yet. A fist does not click; thumbs-up exits. The other hand remains independent. Loss of hand or head tracking exits too.',
        'В активном тачпаде щипок нажимает Trigger, открытая ладонь к себе — Grip, а V — клик панели. Оси продолжают работать вместе с этими жестами. V не нажимает X/A в этой сессии; замены для этих кнопок пока нет. Кулак не нажимает панель, большой вверх — выход. Вторая рука работает независимо. Потеря трекинга руки или головы тоже выключает панель.')+'</p>'
    simple+='<p>'+tr('To disable only the panel on Linux, add SIMPLE_GESTURES_TOUCHPAD=0 to the launch command. In the Windows launcher’s .env, set XR_SIMPLE_GESTURES_TOUCHPAD=0. Restart tracking after changing it.',
        'Чтобы отключить только панель в Linux, добавь SIMPLE_GESTURES_TOUCHPAD=0 к команде запуска. В .env Windows-лаунчера укажи XR_SIMPLE_GESTURES_TOUCHPAD=0. После изменения перезапусти трекинг.')+'</p>'
    simple+='<p>'+tr('Default radius: 10 cm. Simple mode adds 30 degrees of outward yaw to the virtual hands and controllers. Set SIMPLE_GESTURES_TOUCHPAD_RADIUS_M and SIMPLE_GESTURES_POSE_YAW_OFFSET_DEGREES in the launch script (0 disables extra yaw). In the Windows .env, prefix these names with XR_. The yaw is skipped while a side uses controller IMU orientation and returns with optical hands.',
        'Радиус по умолчанию: 10 см. Простой режим добавляет 30° наружу виртуальным рукам и контроллерам. Настройки скрипта: SIMPLE_GESTURES_TOUCHPAD_RADIUS_M и SIMPLE_GESTURES_POSE_YAW_OFFSET_DEGREES (0 отключает доворот). В .env Windows добавь к именам префикс XR_. Доворот пропускается, пока сторона использует ориентацию IMU контроллера, и возвращается с оптическими руками.')+'</p>'
    simple+='<p>'+tr('On Linux, stop tracking and restart with:','В Linux останови трекинг и запусти:')+'</p><pre><code>SIMPLE_GESTURES=1 ./run_xr_client.sh --config xreal_ultra</code></pre>'
    return page(tr('Controller gestures','Управление жестами'),intro+tabbar(entries)+
        panel('controller-emulation',finger_map+em)+panel('grab-pinch',simple),ru,style=COMPACT_STYLE)


def touchpad_picture(ru=False):
    # Reuse the project's original vector outline, without the numbered finger map.
    hand=re.sub(r'<g font-family=.*?</g>','',FINGER_MAP_SVG,flags=re.S)
    hand=re.sub(r'<g fill="none" stroke="#617e9f".*?</g>','',hand,flags=re.S)
    hand=re.sub(r'<svg[^>]*>','<g transform="translate(174 108) scale(.46)">',hand,count=1)
    hand=hand.replace('</svg>','</g>').replace('palm-fill','touchpad-hand-fill')
    hand=re.sub(r'<title.*?</title>|<desc.*?</desc>','',hand,flags=re.S)
    hand=hand.replace('palm-title','touchpad-title').replace('palm-desc','touchpad-desc')
    nails='<g fill="#7894ad" stroke="#a7bfd4" stroke-width="2"><rect x="106" y="103" width="23" height="31" rx="8"/><rect x="154" y="49" width="24" height="32" rx="8"/><rect x="203" y="80" width="23" height="31" rx="8"/><rect x="247" y="132" width="21" height="27" rx="7"/></g>'
    hand=hand.rsplit('</g>',1)[0]+nails+'</g>'
    body='<defs><marker id="touchpad-arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="5" markerHeight="5" orient="auto-start-reverse"><path d="M0 0L10 5L0 10Z" fill="#64dfce"/></marker></defs>'
    body+='<circle cx="250" cy="210" r="145" fill="none" stroke="#56758f" stroke-width="2" stroke-dasharray="7 7"/>'+hand
    for path in ['M250 110L250 65','M250 307L250 355','M172 210L105 210','M326 210L395 210']:
        body+=f'<path d="{path}" fill="none" stroke="#64dfce" stroke-width="5" marker-end="url(#touchpad-arrow)"/>'
    for x,y,en,rus in [(250,41,'Up','Вверх'),(250,390,'Down','Вниз'),(58,217,'Left','Влево'),(450,217,'Right','Вправо')]:
        body+=f'<text x="{x}" y="{y}" text-anchor="middle" fill="#e8eff8" font-size="18">{rus if ru else en}</text>'
    body+='<text x="364" y="340" text-anchor="middle" fill="#b9c9de" font-size="17">'+('10 см' if ru else '10 cm')+'</text>'
    caption='Ладонь от себя. Двигай руку относительно головы; кисть не нужно наклонять.' if ru else 'Palm away from you. Move the hand relative to your head; wrist tilt is not required.'
    return '<figure class="touchpad-art">'+svg(body,520,410,title=caption)+'<figcaption>'+caption+'</figcaption></figure>'


def svg(body, width=520, height=330, title='Hand gesture'):
    return f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="{html.escape(title,quote=True)}" xmlns="http://www.w3.org/2000/svg">{body}</svg>'


def card(title,description,picture,tag='',wide=False):
    return f'<article class="card{" wide" if wide else ""}"><div class="art">{picture}</div><div class="copy"><span class="tag">{tag}</span><h3>{title}</h3><p>{description}</p></div></article>'


def controller_picture(side,ru=False):
    image=base64.b64encode((ROOT/'images/gearvr-controller-model.png').read_bytes()).decode()
    side_image=base64.b64encode((ROOT/'images/gearvr-controller-side.png').read_bytes()).decode()
    # One identical licensed model image on both sides, with side-specific labels.
    xa='X' if side=='LEFT' else 'A';yb='Y' if side=='LEFT' else 'B'
    labels=[(16,90,350,115,'Touchpad → stick X/Y'),(16,200,312,195,'Back → Grip'),
        (493,200,355,191,'Home → Menu'),(16,266,334,235,f'Volume + → {xa}'),
        (493,292,333,283,f'Volume − → {yb}'),(16,368,344,145,'Press → stick click')]
    body=f'<text x="350" y="28" fill="#64dfce" text-anchor="middle" font-size="28" font-weight="700">{side}</text>'
    body+=f'<image href="data:image/png;base64,{image}" x="200" y="40" width="300" height="380"/>'
    body+=f'<image href="data:image/png;base64,{side_image}" x="490" y="380" width="130" height="165"/>'
    for x,y,tx,ty,text in labels:
        endx=x+170 if x<100 else x-10
        body+=f'<path d="M{endx} {y-7}L{tx} {ty}" stroke="#64dfce" stroke-width="1.8" fill="none"/>'
        body+=f'<circle cx="{tx}" cy="{ty}" r="4" fill="#64dfce"/>'
        body+=f'<text x="{x}" y="{y}" fill="#e9f1fc" font-size="16">{html.escape(text)}</text>'
    body+='<text x="350" y="465" text-anchor="middle" fill="#bccde1" font-size="16">Touch surface → stick touch</text>'
    body+='<text x="490" y="365" fill="#e9f1fc" font-size="16">Rear trigger → Trigger</text><path d="M550 370L583 404" stroke="#64dfce" stroke-width="1.8"/><circle cx="583" cy="404" r="4" fill="#64dfce"/>'
    return svg(body,700,560,title='Samsung Gear VR '+side+' SteamVR mapping')


def support(ru=False):
    def tr(en,rus):return rus if ru else en
    intro='<p>'+tr('Default XR Gate → SteamVR mapping. The same Samsung Gear VR Controller works on either side; assign one to LEFT and the other to RIGHT in the launcher.',
        'Стандартная раскладка XR Gate → SteamVR. Одинаковые Samsung Gear VR Controller используются с обеих сторон: назначь один LEFT, другой RIGHT в лаунчере.')+'</p>'
    content=intro+'<div class="grid mapping">'+card('LEFT',tr('Volume + is X; volume − is Y.','Громкость + — X, громкость − — Y.'),controller_picture('LEFT',ru))
    content+=card('RIGHT',tr('Volume + is A; volume − is B.','Громкость + — A, громкость − — B.'),controller_picture('RIGHT',ru))+'</div>'
    content+='<h2>'+tr('Tracking and motion prediction','Трекинг и предсказание движения')+'</h2><p>'+tr(
        'XR Gate combines optical hand tracking with the controller’s IMU. Its gyroscope and accelerometer improve orientation tracking and motion prediction through brief optical tracking gaps. Keep the controller in the tracked hand.',
        'XR Gate совмещает оптический трекинг руки с IMU контроллера. Гироскоп и акселерометр помогают отслеживать ориентацию и предсказывать движение при кратких пропусках оптического трекинга. Держи контроллер в отслеживаемой руке.')+'</p>'
    content+='<p>'+tr('The arrows show XR Gate’s standard inputs. SteamVR applications may bind these inputs to different actions. Custom controller profiles may change the mapping.',
        'Стрелки показывают стандартные входы XR Gate. Приложения SteamVR могут назначать им разные действия. Пользовательский профиль контроллера может менять раскладку.')+'</p>'
    content+='<p class="notice">'+tr('Model: WebXR Input Profiles assets, © 2019 Amazon, MIT. Rendered and annotated for XR Gate. ',
        'Модель: WebXR Input Profiles assets, © 2019 Amazon, MIT. Рендер и подписи подготовлены для XR Gate. ')+ '<a href="https://github.com/immersive-web/webxr-input-profiles/tree/main/packages/assets/profiles/samsung-gearvr">Source</a> · <a href="https://github.com/immersive-web/webxr-input-profiles/blob/main/packages/assets/LICENSE.md">MIT license</a></p>'
    return page('Support VR controllers',tabbar([('samsung-gearvr','Samsung Gear VR Controller')])+panel('samsung-gearvr',content),ru,True)


def main():
    for ru in (False,True):
        suffix='.ru' if ru else ''
        (ROOT/f'GESTURES{suffix}.html').write_text(gestures(ru),encoding='utf-8')
        (ROOT/f'SUPPORT-VR-CONTROLLERS{suffix}.html').write_text(support(ru),encoding='utf-8')


if __name__=='__main__':main()
