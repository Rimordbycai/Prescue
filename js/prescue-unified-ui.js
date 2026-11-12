/* PRESCUE Wi-Fi + USB Unified Mode. Data live, not a simulator. */
(function () {
  'use strict';
  const api=window.PRESCUE_API;
  const form=document.getElementById('apiForm');
  if(!api||!form)return;
  const button=document.createElement('button');
  button.type='button';button.className='button secondary';
  button.textContent='Gunakan Wi-Fi + USB Terpadu';
  button.id='pcUnifiedMode';
  const note=document.createElement('p');
  note.style.cssText='font-size:13px;opacity:.85;margin:8px 0;';
  note.textContent='Satu API lokal: tekanan dan status pompa/katup via Wi-Fi; fase, hasil uji, dan input via USB. Bridge harus berjalan.';
  form.append(button,note);
  function useUnified(){
    const url='http://127.0.0.1:8000';
    api.setSettings({baseUrl:url,statusPath:'/api/status',samplesPath:'/api/samples',mode:'live'});
    document.getElementById('apiBaseUrl').value=url;
    document.getElementById('apiStatusPath').value='/api/status';
    document.getElementById('apiSamplesPath').value='/api/samples';
    document.getElementById('modeLiveBtn').click();
    const feedback=document.getElementById('apiFeedback');
    feedback.textContent='Mode terpadu dipilih. Status kartu mengikuti sensor asli; pengendalian fisik tetap memerlukan ARM BENCH dan izin operator.';
  }
  button.addEventListener('click',useUnified);
  const toggle=document.getElementById('pcUsbV2Enabled');
  if(toggle)toggle.addEventListener('change',()=>{if(toggle.checked)useUnified();});
})();
