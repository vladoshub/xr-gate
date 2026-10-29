#!/usr/bin/env python3
"""Build offline input guides with original SVG hand drawings and a licensed model render.

Only the Python standard library is needed. User reference photographs are
never read, embedded, copied or distributed by this generator.
"""
import base64
import html
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
SKIN='#edb58f'; EDGE='#995f49'; ACCENT='#64dfce'


def hand(extended=(0,1), thumb='neutral', active=None, back=False, mirror=False, angle=0):
    """Original instructional illustration; finger indices: index, middle, ring, little."""
    wrap=f'<g transform="translate(260 0) scale(-1 1)">' if mirror else '<g>'
    out=[wrap,f'<g transform="rotate({angle} 130 170)">',
        f'<path d="M91 231L88 291Q129 301 174 289L177 229Z" fill="{SKIN}" stroke="{EDGE}" stroke-width="2.5"/>',
        f'<path d="M82 237Q64 218 65 175L76 122Q82 103 100 108L180 113Q203 124 207 156L202 204Q195 235 174 246L104 247Z" fill="{SKIN}" stroke="{EDGE}" stroke-width="2.5"/>']
    bases=[(94,127),(127,117),(161,124),(190,144)]
    tips=[(86,35),(127,14),(169,31),(205,69)]
    for i,((x,y),(tx,ty)) in enumerate(zip(bases,tips)):
        if i==0 and thumb=='circle':
            d=f'M{x} {y} C{x-1} {y-38} 71 72 58 94 Q49 107 57 119'; tip=(57,119)
        elif i in extended:
            d=f'M{x} {y+9} Q{(x+tx)/2} {(y+ty)/2} {tx} {ty}';tip=(tx,ty)
        else:
            d=f'M{x} {y} Q{x-3} {y-22} {x+9} {y-12} C{x+28} {y-1} {x+20} {y+35} {x+3} {y+39}';tip=(x+3,y+39)
        color=ACCENT if active==i else EDGE
        out += [f'<path d="{d}" fill="none" stroke="{color}" stroke-width="27" stroke-linecap="round" stroke-linejoin="round"/>',
                f'<path d="{d}" fill="none" stroke="{SKIN}" stroke-width="22" stroke-linecap="round" stroke-linejoin="round"/>']
        if i not in extended and not(i==0 and thumb=='circle'):
            out.append(f'<ellipse cx="{tip[0]}" cy="{tip[1]-3}" rx="8" ry="10" fill="#f6d0b9" stroke="#bd8b74"/>')
        elif back:
            out.append(f'<rect x="{tip[0]-7}" y="{tip[1]-5}" width="14" height="20" rx="6" fill="#f6d0b9" stroke="#bd8b74"/>')
        elif i in extended:
            out.append(f'<path d="M{x-7} {y-19}l15 -1 M{x-7} {y-26}l14 -1" stroke="#ba8267" stroke-width="1.5"/>')
    thumbs={
        'neutral':'M78 183 Q56 158 53 138 Q48 121 62 114',
        'out':'M78 183 Q45 157 25 127 L13 103',
        'forward':'M78 183 Q44 154 18 139 Q6 132 13 117',
        'back':'M78 183 Q46 154 23 119 L12 81 Q10 68 25 74 L66 104',
        'left':'M78 183 Q44 164 21 156 L5 150',
        'right':'M78 183 Q55 152 61 127 L94 104',
        'tucked':'M78 184 Q99 179 118 162 L143 147',
        'circle':'M78 183 Q45 160 45 141 Q44 124 57 119',
        'up':'M78 183 Q56 151 54 124 L50 81'}
    d=thumbs[thumb]
    out += [f'<path d="{d}" fill="none" stroke="{ACCENT if active=="thumb" else EDGE}" stroke-width="31" stroke-linecap="round" stroke-linejoin="round"/>',
            f'<path d="{d}" fill="none" stroke="{SKIN}" stroke-width="26" stroke-linecap="round" stroke-linejoin="round"/>']
    if thumb=='back':
        out.append('<path d="M9 91Q22 85 33 96" fill="none" stroke="#a9765f" stroke-width="2"/><ellipse cx="61" cy="101" rx="10" ry="7" fill="#f7d3bc" stroke="#ba8d75" transform="rotate(35 61 101)"/>')
    if not back:
        out.append('<path d="M91 187Q113 164 157 167 M105 207Q139 223 176 202 M88 191Q78 219 112 232" fill="none" stroke="#bf866b" stroke-width="2.2" opacity=".75"/>')
    out.append('</g></g>')
    return ''.join(out)


def svg(body, width=520, height=330, title='Hand gesture'):
    return f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="{html.escape(title,quote=True)}" xmlns="http://www.w3.org/2000/svg">{body}</svg>'


def one_hand(**kwargs):
    return svg('<g transform="translate(125 8)">'+hand(**kwargs)+'</g>')


def neutral_pair(ru=False):
    text='РЕАЛЬНЫЕ РУКИ — К ТЕЛУ' if ru else 'REAL HANDS — TURN INWARD'
    virtual='В VR — ВПЕРЁД' if ru else 'IN VR — POINT FORWARD'
    body='<g transform="translate(5 15) scale(.75)">'+hand(mirror=True,angle=-35)+'</g>'
    body+='<g transform="translate(320 15) scale(.75)">'+hand(angle=-35)+'</g>'
    body+='<path d="M235 117Q260 145 285 117" fill="none" stroke="#64dfce" stroke-width="3"/>'
    body+='<path d="M237 111l-3 12 12-2 M283 111l3 12-12-2" fill="none" stroke="#64dfce" stroke-width="3"/>'
    body+=f'<text x="260" y="280" text-anchor="middle" fill="#e9f1fc" font-size="15">{text}</text>'
    body+=f'<text x="260" y="307" text-anchor="middle" fill="#64dfce" font-size="15">{virtual}</text>'
    return svg(body)


def menu_picture(ru=False):
    text='ЛАДОНИ ОТ СЕБЯ' if ru else 'PALMS AWAY FROM YOU'
    return svg('<g transform="translate(15 10) scale(.85)">'+hand((0,1,2,3),'out',back=True,mirror=True)+'</g>'
        +'<g transform="translate(285 10) scale(.85)">'+hand((0,1,2,3),'out',back=True)+'</g>'
        +f'<text x="260" y="310" text-anchor="middle" fill="#64dfce" font-size="17">{text}</text>')


def thumb_picture(pose,ru=False):
    # The magnified thumb keeps the palm as a fixed reference.
    body='<g transform="translate(50 -95) scale(1.45)">'+hand(thumb=pose,active='thumb')+'</g>'
    arrows={
      'forward':'M380 180L380 110 M368 123L380 110L392 123',
      'back':'M380 110L380 180 M368 167L380 180L392 167',
      'left':'M412 145L347 145 M360 133L347 145L360 157',
      'right':'M347 145L412 145 M399 133L412 145L399 157'}
    if pose in arrows:
        body+=f'<path d="{arrows[pose]}" stroke="#64dfce" stroke-width="5" fill="none" stroke-linecap="round" stroke-linejoin="round"/>'
    return svg(body,height=250,title='Thumb '+pose)


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


def page(title,contents,ru=False,support=False):
    suffix='.ru' if ru else ''
    other='' if ru else '.ru'
    filename='SUPPORT-VR-CONTROLLERS' if support else 'GESTURES'
    return f'''<!doctype html><html lang="{'ru' if ru else 'en'}"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>{title} — XR Gate</title><style>{STYLE}</style></head><body>
<nav aria-label="{'Справка' if ru else 'Help'}"><a href="START-HERE{suffix}.html">XR Gate</a><a href="GESTURES{suffix}.html">Gestures</a><a href="SUPPORT-VR-CONTROLLERS{suffix}.html">Support VR controllers</a><a href="{filename}{other}.html" lang="{'en' if ru else 'ru'}">{'English' if ru else 'Русский'}</a></nav>
<main><h1>{title}</h1>{contents}<footer>XR Gate · <a href="https://github.com/vladoshub/xr-gate">GitHub</a></footer></main>{TABS}</body></html>'''


def tabbar(entries):
    return '<div class="tabs" role="tablist" aria-label="Input guide">'+''.join(
        f'<button type="button" role="tab" id="tab-{id}" data-panel="{id}" aria-controls="{id}" aria-selected="{str(i==0).lower()}">{label}</button>'
        for i,(id,label) in enumerate(entries))+'</div>'


def panel(id,body):
    return f'<section role="tabpanel" id="{id}" aria-labelledby="tab-{id}">{body}</section>'


def card(title,description,picture,tag='',wide=False):
    return f'<article class="card{" wide" if wide else ""}"><div class="art">{picture}</div><div class="copy"><span class="tag">{tag}</span><h3>{title}</h3><p>{description}</p></div></article>'


def gestures(ru=False):
    def tr(en,rus):return rus if ru else en
    entries=[('controller-emulation','Gestures (controller emulation)'),('grab-pinch','Gestures (grab / pinch)')]
    intro='<p class="intro">'+tr('Choose the same mode in the launcher’s Controllers list. Controller emulation is the default. Keep your hands visible to the glasses’ cameras.',
        'Выбери такой же режим в списке Controllers лаунчера. По умолчанию используется эмуляция контроллеров. Держи руки в поле зрения камер очков.')+'</p>'
    em='<div class="grid">'
    em+=card(tr('Neutral / ready position','Нейтральное положение'),tr(
        'Keep the index and middle fingers straight, ring finger and pinky bent, and thumb relaxed. Turn your wrists inward toward your body so the cameras see the fingers clearly. The configured yaw offset turns the virtual controllers forward in VR.',
        'Указательный и средний прямые, безымянный и мизинец согнуты, большой расслаблен. Поверни кисти к телу для лучшего распознавания пальцев. Настроенное смещение yaw доворачивает виртуальные контроллеры вперёд в VR.'),neutral_pair(ru),tr('Start here','Начни с этой позы'),True)
    em+=card('Trigger',tr('Bend your index finger. Straighten it to release. You can try a partial bend.',
        'Согни указательный. Выпрями, чтобы отпустить. Можно попробовать сгибать не до конца.'),one_hand(extended=(1,),active=0),tr('Both hands','Обе руки'))
    em+=card('Grip',tr('Bend your middle finger. Straighten it to release. You can try a partial bend.',
        'Согни средний. Выпрями, чтобы отпустить. Можно попробовать сгибать не до конца.'),one_hand(extended=(0,),active=1),tr('Both hands','Обе руки'))
    em+=card('X / A',tr('Touch thumb and index fingertips to make a circle. Keep the other three fingers straight. Left: X. Right: A.',
        'Соедини большой и указательный в кольцо. Остальные три пальца прямые. Слева — X, справа — A.'),one_hand(extended=(1,2,3),thumb='circle',active=0),tr('Face button','Кнопка'))
    em+=card('Y / B',tr('Make the same thumb–index circle and bend the other three fingers. Left: Y. Right: B.',
        'Сделай такое же кольцо, а остальные три пальца согни. Слева — Y, справа — B.'),one_hand(extended=(),thumb='circle',active=0),tr('Face button','Кнопка'))
    em+=card(tr('Touchpad / thumbstick click','Нажатие тачпада / стика'),tr(
        'Open one palm toward yourself, with all five fingers straight and spread. You see your palm. Return to the ready position to release. Movement, Trigger and Grip pause while showing this separate gesture.',
        'Раскрой одну ладонь к себе: все пять пальцев прямые и разведены. Ты видишь свою ладонь. Вернись в нейтральную позу для отпускания. На время этого отдельного жеста движение, Trigger и Grip приостанавливаются.'),one_hand(extended=(0,1,2,3),thumb='out'),tr('Palm toward you','Ладонь к себе'))
    em+=card('Menu',tr('Hold both hands open, palms facing away from you, like a stop sign. You see the backs of your hands. Hold briefly; both hands must remain visible.',
        'Раскрой обе ладони от себя, как знак «стоп». Ты видишь тыльную сторону рук. Ненадолго удерживай позу; обе руки должны оставаться видны камерам.'),menu_picture(ru),tr('Both hands together','Две руки одновременно'))
    em+='</div><h2>'+tr('Movement with your thumb','Движение большим пальцем')+'</h2><p>'+tr(
        'Keep your wrist still. These drawings show the thumb poses; the other fingers stay in the ready position. Trigger and Grip can work together with movement. Diagonals combine sideways movement with forward or backward.',
        'Кисть держи неподвижно. На рисунках показаны положения большого; остальные пальцы остаются в нейтральной позе. Trigger и Grip могут работать вместе с движением. Для диагоналей совмещай боковое направление с движением вперёд или назад.')+'</p><div class="grid thumb-grid">'
    for pose,title,description in [
        ('neutral',tr('Neutral — no movement','Нейтраль — стоим'),tr('Keep your thumb relaxed and slightly away from your palm.','Большой расслаблен и немного отведён от ладони.')),
        ('forward',tr('Forward','Вперёд'),tr('Straighten and slightly arch your thumb away from your palm.','Выпрями и слегка выгни большой от ладони.')),
        ('back',tr('Backward','Назад'),tr('Curl the tip toward your palm while keeping the base away. Make the bend clear.','Согни кончик к ладони, оставляя основание отведённым. Покажи изгиб отчётливо.')),
        ('left',tr('Left','Влево'),tr('Sweep your thumb to the left without turning your wrist.','Отведи большой влево, не поворачивая кисть.')),
        ('right',tr('Right','Вправо'),tr('Sweep your thumb to the right without turning your wrist.','Отведи большой вправо, не поворачивая кисть.')),
        ('tucked',tr('D-pad off','D-pad выключен'),tr('Press your thumb against your palm to stop movement.','Плотно прижми большой к ладони, чтобы выключить движение.'))]:
        em+=card(title,description,thumb_picture(pose,ru),tr('Thumb D-pad','D-pad большим'))
    em+='</div><div class="notice">'+tr('If wrist D-pad is selected in your profile, keep your thumb out and tilt your wrist in the direction you want to move. Tuck the thumb to stop. Thumb and wrist modes are alternatives.',
        'Если в профиле выбран D-pad от кисти, отведи большой и наклоняй кисть в нужную сторону. Прижми большой для остановки. Режимы большого и кисти выбираются по отдельности.')+'</div>'
    em+='<details><summary>'+tr('Wrist D-pad poses (optional mode)','Положения кисти для D-pad (дополнительный режим)')+'</summary><div class="grid">'
    for title,angle,path in [(tr('Tilt forward','Наклон вперёд'),-15,'M430 160L430 100 M419 113L430 100L441 113'),
            (tr('Tilt backward','Наклон назад'),15,'M430 100L430 160 M419 147L430 160L441 147'),
            (tr('Tilt left','Наклон влево'),-35,'M460 130L400 130 M413 119L400 130L413 141'),
            (tr('Tilt right','Наклон вправо'),35,'M400 130L460 130 M447 119L460 130L447 141')]:
        picture=svg('<g transform="translate(110 5)">'+hand(thumb='out',angle=angle)+'</g>'+f'<path d="{path}" stroke="#64dfce" stroke-width="4" fill="none" stroke-linecap="round"/>')
        em+=card(title,tr('Keep the thumb away from the palm. Tilt the wrist; combine two directions for a diagonal.',
            'Большой отведён от ладони. Наклоняй кисть; совмести два направления для диагонали.'),picture,tr('Wrist mode only','Только режим кисти'))
    em+='</div></details>'
    simple='<p>'+tr('The original hand gestures use the same mapping on either hand: pinch for Trigger and grab for Grip. Open your hand to release. There is no thumb D-pad or open-palm touchpad gesture in this mode.',
        'Исходные жесты одинаковы на обеих руках: щипок — Trigger, хват — Grip. Раскрой руку для отпускания. D-pad большим и отдельный клик раскрытой ладонью здесь не используются.')+'</p><div class="grid">'
    simple+=card(tr('Pinch → Trigger','Щипок → Trigger'),tr('Bring your thumb and index fingertips together. Separate them to release.',
        'Сведи кончики большого и указательного. Разведи их для отпускания.'),one_hand(extended=(1,2,3),thumb='circle'),tr('Both hands','Обе руки'))
    simple+=card(tr('Grab → Grip','Хват → Grip'),tr('Bend the middle, ring and little fingers to grab. Open them to release.',
        'Согни средний, безымянный и мизинец для хвата. Разогни их для отпускания.'),one_hand(extended=(),thumb='tucked'),tr('Both hands','Обе руки'))
    simple+=card(tr('Open hand → release','Раскрытая рука → отпускание'),tr('Relax and open your hand to release the grab and pinch.',
        'Раскрой руку и разомкни щипок, чтобы отпустить кнопки.'),one_hand(extended=(0,1,2,3),thumb='out'),tr('Released','Отпущено'))
    simple+='</div><h2>'+tr('Additional legacy bindings','Дополнительные старые жесты')+'</h2><p>'+tr('If enabled in the runtime profile, thumbs-up and pointing provide additional A/B inputs. These shape gestures can overlap with Grip; application bindings determine their action.',
        'Если они включены в профиле, большой вверх и указательный вперёд дают дополнительные входы A/B. Эти формы руки могут совмещаться с Grip; действие зависит от раскладки приложения.')+'</p><div class="grid">'
    simple+=card(tr('Thumbs-up → A input','Большой вверх → вход A'),tr('Make a fist and extend your thumb.','Сожми кулак и отогни большой.'),one_hand(extended=(),thumb='up'),tr('Profile-dependent','Зависит от профиля'))
    simple+=card(tr('Point → B input','Указательный вперёд → вход B'),tr('Straighten only your index finger; bend the other three.','Выпрями указательный, остальные три пальца согни.'),one_hand(extended=(0,),thumb='neutral'),tr('Profile-dependent','Зависит от профиля'))
    simple+='</div>'
    return page(tr('Controller gestures','Управление жестами'),intro+tabbar(entries)+panel('controller-emulation',em)+panel('grab-pinch',simple),ru)


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
