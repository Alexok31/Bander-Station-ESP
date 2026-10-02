"""Create an offline design preview using sample values, never device credentials."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
def preview():
    values = dict(CONNECTION_CLASS='', CONNECTION_TEXT='Wi-Fi подключён', IP='192.168.4.1',
                  CURRENT_STATION='Majestic Jukebox', RADIO_STATUS='FM / Интернет-радио · на паузе',
                  CUSTOM_COUNT='0', STATION_MAX='16', STATIONS='', SSID='Домашняя сеть',
                  AP_SSID='Bender Station', AI_URL='', WAKE_CHECKED='checked', SHAKE='11000',
                  SHAKE_MIN='4000', SHAKE_MAX='24000', SHAKE_STEP='500', SHAKE_DEFAULT='11000',
                  SARCASM='80', SOCIABILITY='55', CURIOSITY='40', STUBBORNNESS='70', WARMTH='35')
    values['CALM_OPTIONS'] = ''.join(f'<option value="{n}"{" selected" if n == 5 else ""}>{str(n)+" мин" if n else "Не засыпать"}</option>' for n in (2,5,10,15,0))
    names = ['Рот 1','Рот 2','Рот 3','Левый глаз','Правый глаз']
    controls = '<div class="head-wrap"><div class="head-row">'
    for i in (3,4,0,1,2):
        if i == 0: controls += '</div><div class="head-row">'
        controls += f'<div class="mx"><div class="mx-title">{names[i]}</div><input type="hidden" name="mbr{i}" id="mbr{i}" value="3"><div class="mx-value" id="v{i}">3</div><div class="mx-buttons">'
        for d in (-1,1): controls += f'<button type="button" data-matrix="{i}" data-delta="{d}" aria-label="{names[i]}: {"темнее" if d < 0 else "ярче"}">{"−" if d < 0 else "+"}</button>'
        controls += '</div></div>'
    values['MATRIX_CONTROLS'] = controls + '</div></div>'
    source = (ROOT/'BendeRadio/webui/index.html').read_text(encoding='utf-8')
    html = re.sub(r'\{\{([A-Z_]+)\}\}',lambda m:values[m[1]],source)
    out = ROOT/'artifacts/webui-preview'
    out.mkdir(parents=True,exist_ok=True)
    # Preview forms never navigate to or modify a device.
    guard = '<script>document.getElementById("preview-character").addEventListener("click",e=>{e.stopImmediatePropagation();alert("Локальный макет: прослушивание работает на колонке.");},true);document.querySelectorAll("form").forEach(f=>f.addEventListener("submit",e=>{e.preventDefault();alert("Локальный предпросмотр: настройки не отправлены.");}));</script>'
    (out/'index.html').write_text(html.replace('</body>',guard+'</body>'),encoding='utf-8')
    checks = r'''<script>(async()=>{
    window.fetch=async()=>({ok:true});
    const checks=[];function check(name,ok){checks.push({name,ok:!!ok});}
    location.hash='settings';showPanel();check('settings navigation',!document.getElementById('panel-settings').hidden&&document.getElementById('panel-radio').hidden);
    shake.value=18000;shake.dispatchEvent(new Event('input',{bubbles:true}));check('slider value',shakeValue.value==='18000');
    document.getElementById('reset-shake').click();check('reset to 11000',shake.value==='11000'&&shakeValue.value==='11000');
    const behavior=document.getElementById('wake_on_shake').form;const wake=document.getElementById('wake_on_shake');wake.checked=false;check('unchecked wake omitted',!new FormData(behavior).has('wake_on_shake'));
    const form=new FormData(behavior);check('behavior independent',form.get('section')==='behavior'&&!form.has('sta_ssid')&&!form.has('stations'));
    document.getElementById('calm_minutes').value='0';check('never sleep submitted',new FormData(behavior).get('calm_minutes')==='0');
    setV(0,15);document.querySelector('[data-matrix="0"][data-delta="1"]').click();check('brightness max',getV(0)===15);document.getElementById('start-calib').click();check('brightness reset',[0,1,2,3,4].every(i=>getV(i)===0));
    location.hash='character';showPanel();check('character navigation',!document.getElementById('panel-character').hidden);
    check('saved preset restored',document.getElementById('character-preset').textContent==='Классический');
    characterPresets.forEach(p=>{p.click();check('preset '+p.dataset.preset,traitInputs.map(i=>i.value).join(',')===p.dataset.values&&p.getAttribute('aria-pressed')==='true');});
    traitInputs[0].value='43';traitInputs[0].dispatchEvent(new Event('input'));check('custom mix',document.getElementById('character-preset').textContent==='Свой микс'&&document.getElementById('sarcasm-value').value==='43');
    const personality=new FormData(document.getElementById('character-form'));check('personality independent',personality.get('section')==='character'&&personality.get('sarcasm')==='43'&&Array.from(personality.keys()).length===6);
    document.getElementById('reset-character').click();check('character reset',traitInputs.map(i=>i.value).join(',')===savedCharacter&&document.getElementById('character-status').textContent==='Сохранённый характер.');
    location.hash='radio';showPanel();check('radio navigation',!document.getElementById('panel-radio').hidden);
    const labels={classic:'Вжарить',kind:'Давай поболтаем',grumpy:'Ну, сука, тестируй',explorer:'А что, если нажать?',calm:'Давай поболтаем'};
    characterPresets.forEach(p=>{p.click();check('preview label '+p.dataset.preset,previewButton.textContent===labels[p.dataset.preset]);});
    document.querySelector('[data-preset="grumpy"]').click();
    previewScenario.value='support';previewScenario.dispatchEvent(new Event('change'));check('question scenario selection',previewQuestion.value.includes('Підтримаєш'));
    const customQuestion='Бендере, <що> скажеш & чому?';previewQuestion.value=customQuestion;previewQuestion.dispatchEvent(new Event('input'));check('custom question selection',previewScenario.value==='custom');
    let posted=null;
    window.fetch=async(url,options)=>{if(options.method==='POST'){posted=new URLSearchParams(options.body);return {ok:true};}return {ok:true,json:async()=>({state:4})};};
    await previewCharacter();check('preview uses unsaved sliders',posted.get('sarcasm')==='90'&&posted.get('warmth')==='15'&&!posted.has('section'));
    check('custom question sent verbatim',posted.get('question')===customQuestion);
    check('preview finishes and keeps dirty values',!previewButton.disabled&&document.getElementById('preview-status').textContent.includes('Вот так')&&document.getElementById('sarcasm').value==='90');
    window.fetch=async()=>({ok:false,text:async()=>'Бендер сейчас занят.'});await previewCharacter();check('preview busy error',!previewButton.disabled&&document.getElementById('preview-status').textContent==='Бендер сейчас занят.');
    let emptyCalls=0;window.fetch=async()=>{emptyCalls++;return {ok:true};};previewQuestion.value='  ';await previewCharacter();check('empty question stays local',emptyCalls===0&&!previewButton.disabled&&document.getElementById('preview-status').textContent.includes('Напиши вопрос'));
    previewQuestion.value=customQuestion;previewQuestion.dispatchEvent(new Event('input'));
    let resolvePost,calls=0;window.fetch=async(url,options)=>{calls++;if(options.method==='POST')return new Promise(r=>resolvePost=r);return {ok:true,json:async()=>({state:5})};};
    const pending=previewCharacter();await previewCharacter();check('duplicate click blocked',calls===1&&previewButton.disabled);resolvePost({ok:true});await pending;
    check('preview failure releases button',!previewButton.disabled&&document.getElementById('preview-status').textContent.includes('Проба прервана'));
    const p=document.createElement('pre');p.id='ui-test-results';p.textContent=JSON.stringify(checks);document.body.append(p);
    })();</script>'''
    (out/'checks.html').write_text(html.replace('</body>',checks+'</body>'),encoding='utf-8')
    print(out/'index.html')

if __name__ == '__main__': preview()
