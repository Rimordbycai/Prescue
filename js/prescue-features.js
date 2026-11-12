
(function(){
  'use strict';
  const el = id => document.getElementById(id);
  const api = window.PRESCUE_API;
  if (!api || !el('pfApply')) return;

  const presets = {
    normal: [120,80,75,'Normal'],
    high: [150,100,80,'Hipertensi'],
    low: [90,60,70,'Hipotensi']
  };

  let selected = 'normal';
  const value = id => Number(el(id).value);
  const fmt = n => Number.isFinite(n) ? n.toFixed(2) + ' V' : 'Belum tersedia';

  function setPreset(key){
    selected = key;
    if(presets[key]){
      const p = presets[key];
      el('pfSbp').value = p[0];
      el('pfDbp').value = p[1];
      el('pfBpm').value = p[2];
    }
    document.querySelectorAll('[data-pf-preset]').forEach(b=>{
      b.classList.toggle('pf-selected',b.dataset.pfPreset===key);
    });
    updatePreview();
  }

  function updatePreview(){
    const sbp=value('pfSbp'), dbp=value('pfDbp'), bpm=value('pfBpm');
    if(!Number.isInteger(sbp)||!Number.isInteger(dbp)||
       !Number.isInteger(bpm)||sbp<50||sbp>200||
       dbp<30||dbp>150||sbp<=dbp||
       bpm<30||bpm>180){
      el('pfPresetResult').textContent='Periksa SBP, DBP, dan nadi. SBP harus lebih tinggi dari DBP.';
      return;
    }
    const name=presets[selected]?.[3]||'Kustom';
    el('pfPresetResult').textContent=
      name+' · '+sbp+'/'+dbp+' mmHg · '+bpm+' bpm · Belum dikirim ke alat';
    el('pfOledMode').textContent='MODE DENYUT · '+name.toUpperCase();
    el('pfOledMain').textContent=sbp+' / '+dbp;
    el('pfOledSub').textContent='NADI '+bpm+' BPM';
    el('pfOledStatus').textContent='PRATINJAU · BELUM ADA PEMBANGKIT DENYUT';
  }

  document.querySelectorAll('[data-pf-preset]').forEach(button=>{
    button.addEventListener('click',()=>setPreset(button.dataset.pfPreset));
  });

  ['pfSbp','pfDbp','pfBpm'].forEach(id=>{
    el(id).addEventListener('input',()=>{
      selected='custom';
      document.querySelectorAll('[data-pf-preset]').forEach(b=>{
        b.classList.toggle('pf-selected',b.dataset.pfPreset==='custom');
      });
    });
  });

  el('pfApply').addEventListener('click',updatePreview);

  el('pfStaticApply').addEventListener('click',()=>{
    const n=value('pfStaticTarget');
    if(!Number.isInteger(n)||n<20||n>150){
      el('pfStaticResult').textContent='Target harus bilangan bulat 20–150 mmHg.';
      return;
    }
    el('pfStaticResult').textContent=
      'Pratinjau tekanan statis '+n+' mmHg. Tidak dikirim ke alat.';
    el('pfOledMode').textContent='TEKANAN STATIS';
    el('pfOledMain').textContent=n+' mmHg';
    el('pfOledSub').textContent='TARGET KONFIGURASI';
    el('pfOledStatus').textContent='PRATINJAU · POMPA TIDAK DIAKTIFKAN';
  });

  function numberField(obj,keys){
    for(const k of keys){
      if(obj[k]!==null && obj[k]!==undefined && obj[k]!==''){
        const n=Number(obj[k]);
        if(Number.isFinite(n))return n;
      }
    }
    return null;
  }

  function clearTelemetry(message){
    el('pfBattery').textContent='Belum tersedia';
    el('pfSensorSupply').textContent='Belum tersedia';
    el('pfBatteryState').textContent=message;
    el('pfSensorState').textContent=message;
    el('pfSafetyState').textContent=message;
    el('pfCalibration').textContent='Belum tersedia';
    el('pfOledBattery').textContent='--.- V';
  }

  let updating=false;

  async function updateTelemetry(){
    const mode=api.getSettings().mode;

    if(mode!=='live'){
      clearTelemetry('Mode Rekaman');
      return;
    }

    if(updating)return;
    updating=true;

    try{
      const raw=await api.requestJSON(api.getSettings().statusPath);
      const d=raw && raw.data && typeof raw.data==='object'
        ? raw.data : raw;

      if(!d || d.connected!==true || d.is_simulated===true){
        clearTelemetry('ESP32 belum terverifikasi');
        return;
      }

      const vb=numberField(d,[
        'battery_voltage_v','battery_voltage','vbat','battery_v'
      ]);
      const vs=numberField(d,[
        'sensor_supply_v','vs_voltage_v','vs_v'
      ]);
      const k=numberField(d,[
        'calibration_k','divider_ratio_k'
      ]);

      el('pfBattery').textContent=fmt(vb);
      el('pfSensorSupply').textContent=fmt(vs);
      el('pfOledBattery').textContent=
        Number.isFinite(vb)?vb.toFixed(1)+' V':'--.- V';

      el('pfBatteryState').textContent=
        vb===null ? 'Belum tersedia'
        : vb<9.9 ? 'Di bawah ambang rancangan penguncian'
        : vb<=10.8 ? 'Di bawah ambang peringatan'
        : 'Di atas ambang peringatan';

      el('pfSensorState').textContent=
        d.sensor_ok===true?'Terbaca menurut firmware'
        :d.sensor_ok===false?'Tidak siap'
        :'Belum tersedia';

      el('pfSafetyState').textContent=
        d.safety_ok===true?'Dilaporkan aktif · belum divalidasi'
        :d.safety_ok===false?'Belum diizinkan'
        :'Belum tersedia';

      el('pfCalibration').textContent=
        k===null?'Belum tersedia':k.toFixed(4);
    }catch(_){
      clearTelemetry('ESP32 tidak terhubung');
    }finally{
      updating=false;
    }
  }

  setPreset('normal');
  updateTelemetry();
  setInterval(updateTelemetry,3000);
})();
