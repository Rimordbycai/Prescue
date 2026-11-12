(function(){
  "use strict";
  const $=id=>document.getElementById(id);
  const api=window.PRESCUE_API;
  const demo=window.PRESCUE_DEMO;
  const config=window.PRESCUE_CONFIG;
  const chart=window.PRESCUE_CHART.create($("pressureChart"),$("chartTooltip"));
  let mode="demo",status={...demo.status},dataPoints=[...demo.points],pollHandle=null,polling=false,replayTimer=null,latestError=null;
  let lastLiveSampleSecond=0,lastLiveUpdate=0;
  const fnum=(value,digits=1)=>Number.isFinite(value)?value.toLocaleString("id-ID",{minimumFractionDigits:digits,maximumFractionDigits:digits}):"—";
  function setText(id,text){$(id).textContent=text;}
  function showToast(message){const el=$("toast");el.textContent=message;el.hidden=false;clearTimeout(showToast.timer);showToast.timer=setTimeout(()=>el.hidden=true,3800);}
  function setConnection(type,label){const pill=$("connectionPill");pill.className=`connection-pill ${type}`;setText("connectionLabel",label);}
  function valueTag(id,value,state){const el=$(id);el.textContent=value;el.className=`value-tag ${state}`;}
  function renderStatus(s){
    const isDemo=mode==="demo";
    setText("metricPressure",fnum(s.pressure_mmHg,3));
    setText("metricTarget",fnum(s.target_mmHg,Number.isInteger(s.target_mmHg)?0:1));
    setText("metricPhase",s.phase||"—");
    setText("metricLeak",fnum(s.leak_rate_mmHg_min,4));
    setText("metricPressureFoot",isDemo?"Akhir rekaman uji kebocoran":"Pembacaan terakhir dari API ESP32");
    setText("metricPhaseFoot",s.test_mode?`Mode pengujian: ${s.test_mode}`:"Belum dikirim API");
    setText("metricLeakFoot",Number.isFinite(s.leak_rate_mmHg_min)?`Batas ≤ ${config.leakLimitMmhgPerMin} mmHg/menit`:"Hasil uji L belum dikirim API");
    if(isDemo){
      valueTag("statusEsp","Belum live","neutral");valueTag("statusSensor","Terbaca di log","green");
      valueTag("statusPump","OFF (akhir uji)","neutral");valueTag("statusValve","Tertutup (akhir uji)","neutral");
    }else{
      valueTag("statusEsp",s.is_simulated===true?"Simulasi":latestError?"Terputus":"Terhubung",s.is_simulated===true?"neutral":latestError?"red":"green");
      valueTag("statusSensor",s.sensor_ok===true?"Terbaca":s.sensor_ok===false?"Belum siap":"Tidak dikirim",s.sensor_ok===true?"green":s.sensor_ok===false?"red":"neutral");
      valueTag("statusPump",s.pump_on===null?"Tidak dikirim":s.pump_on?"ON":"OFF",s.pump_on?"green":"neutral");
      valueTag("statusValve",s.valve_open===null?"Tidak dikirim":s.valve_open?"Terbuka":"Tertutup",s.valve_open?"green":"neutral");
    }
    setText("statusRaw",Number.isFinite(s.adc_raw)?fnum(s.adc_raw,0):"Belum tersedia");
    setText("statusVolt",Number.isFinite(s.sensor_voltage)?`${fnum(s.sensor_voltage,3)} V`:"Belum tersedia");
    if(isDemo){setText("detailTarget","150 mmHg");}
    setText("dataSourceLabel",isDemo?"REKAMAN PENGUJIAN":"LIVE · ESP32 API");
    setText("graphSubtitle",isDemo?"Cuplikan titik hasil pengujian G dan L · bukan live":"Data realtime dari ESP32 (jika dikirim firmware)");
    setText("chartStatusLabel",isDemo?"Cuplikan data rekaman":"Menunggu/menerima telemetry live");
    $("chartDot").className=`chart-live-dot${!isDemo&&!latestError&&s.is_simulated!==true?" on":""}`;
    setText("chartSamplesCount",`${dataPoints.length} titik ditampilkan`);
    $("replayBtn").disabled=!isDemo;
  }
  function makeTd(text,css=""){const td=document.createElement("td");td.textContent=text;if(css)td.className=css;return td;}
  function renderTables(){
    const tbody=$("historyTbody");tbody.replaceChildren();
    // Historical facts are in demo fixture; live status does not fabricate server-side history.
    if(mode!=="demo"){
      const row=document.createElement("tr"),td=makeTd("Riwayat live belum tersedia. Teman firmware dapat menyediakan GET /api/history.");td.colSpan=5;row.append(td);tbody.append(row);setText("historyCount","Belum tersinkron");
    }else{
      demo.history.forEach(h=>{const row=document.createElement("tr");row.append(makeTd(h.name),makeTd(`${h.target} mmHg`),makeTd(h.phase),makeTd(`${h.duration} detik`));const end=makeTd(h.result,"red-text");row.append(end);tbody.append(row);});setText("historyCount",`${demo.history.length} rekaman`);
    }
    const sbody=$("samplesTbody");sbody.replaceChildren();
    const visible=dataPoints.slice(-35).reverse();visible.forEach(p=>{
      const row=document.createElement("tr");const td=document.createElement("td"),chip=document.createElement("span");chip.className=`phase-chip ${p.phase.toLowerCase()==="fill"?"fill":p.phase.toLowerCase()==="settle"?"settle":"record"}`;chip.textContent=p.phase;td.append(chip);row.append(td,makeTd(fnum(p.seconds,3)),makeTd(fnum(p.pressure,3)));sbody.append(row);
    });
    if(!visible.length){const row=document.createElement("tr"),td=makeTd("Belum ada sampel dari API.");td.colSpan=3;row.append(td);sbody.append(row);}
    setText("logPointCount",`${dataPoints.length} titik`);
  }
  function renderAll(){renderStatus(status);renderTables();chart.setData(dataPoints,mode);}
  function renderModeNotice(){
    const n=$("modeNotice");
    if(mode==="demo"){
      n.className="notice info";
      n.innerHTML='<svg class="ico"><use href="#i-info"/></svg><div><strong>Data pengujian terakhir (rekaman).</strong><span>Angka dan grafik adalah cuplikan hasil tim, bukan koneksi langsung. Sumber live dapat diaktifkan pada menu Integrasi API.</span></div>';
    }else{
      n.className="notice info";
      if(status.is_simulated===true){n.innerHTML='<svg class="ico"><use href="#i-info"/></svg><div><strong>SIMULASI API LOKAL.</strong> Nilai dan grafik bukan pembacaan ESP32. Tidak ada pengendalian perangkat fisik.</div>';return;}
      const first=latestError?`<strong>ESP32 belum terhubung:</strong> ${escapeText(latestError)}`:(status.connected===true&&!Number.isFinite(status.pressure_mmHg)?"<strong>ESP32 terhubung, sensor belum siap.</strong> Menunggu pembacaan tekanan dari rangkaian.":"<strong>Mode Live ESP32 aktif.</strong> Pembacaan melalui HTTP GET.");
      n.innerHTML=`<svg class="ico"><use href="#i-info"/></svg><div>${first}</div>`;
    }
  }
  function escapeText(x){const el=document.createElement("div");el.textContent=String(x);return el.innerHTML;}
  async function pollLive(){
    if(mode!=="live"||polling)return;
    polling=true;
    try{
      const s=await api.getLiveStatus();
      if(s.connected!==true)throw new Error("API belum mengonfirmasi koneksi ESP32");
      latestError=null;status=s;lastLiveUpdate=Date.now();setConnection(s.is_simulated===true?"is-demo":"is-live",s.is_simulated===true?"SIMULASI · Bukan ESP32":"ESP32 · Terhubung");
      const incoming=Array.isArray(s.samples)?api.normalizeSamples(s.samples):null;
      if(incoming&&incoming.length){dataPoints=incoming;lastLiveSampleSecond=Math.max(...incoming.map(x=>x.elapsed));}
      else{
        // Jika firmware hanya mengirim status, riwayat chart digambar dari pembacaan berkala.
        // Elapsed dari uptime/status jika tersedia; jika tidak gunakan waktu sejak sesi browser.
        const sec=Number.isFinite(s.elapsed_seconds)?s.elapsed_seconds:(Date.now()-liveStart)/1000;
        if(Number.isFinite(s.pressure_mmHg)&&Number.isFinite(sec)&&sec>lastLiveSampleSecond){dataPoints.push({phase:String(s.phase||"LIVE"),pressure:s.pressure_mmHg,elapsed:sec,seconds:sec});lastLiveSampleSecond=sec;dataPoints=dataPoints.slice(-config.maxChartPoints);}
      }
      renderAll();renderModeNotice();
      if(api.getSettings().samplesPath.trim()){
        try{const a=await api.getLiveSamples();if(a&&a.length&&mode==="live"){dataPoints=a;lastLiveSampleSecond=Math.max(...a.map(x=>x.elapsed));renderAll();}}
        catch(_){/* opsional: jangan menyamarkan status endpoint utama karena /samples belum dibuat */}
      }
    }catch(e){latestError=e.message||"Koneksi gagal";status={...status,pressure_mmHg:null,target_mmHg:null,phase:null,sensor_ok:null,pump_on:null,valve_open:null};renderStatus(status);setConnection("is-error","ESP32 · Terputus");setText("metricPressure","—");valueTag("statusEsp","Terputus","red");setText("metricPressureFoot","Koneksi putus; nilai terakhir bila tersedia");setText("chartStatusLabel","Terputus — data terakhir, bukan live aktif");$("chartDot").className="chart-live-dot";setText("dataSourceLabel","LIVE · TIDAK TERHUBUNG");renderModeNotice();}
    finally{polling=false;}
  }
  let liveStart=Date.now();
  function stopPolling(){if(pollHandle){clearInterval(pollHandle);pollHandle=null;}}
  function cancelReplay(){if(replayTimer){clearInterval(replayTimer);replayTimer=null;}$("replayBtn").innerHTML='<svg class="ico"><use href="#i-play"/></svg> Putar ulang rekaman';}
  function setMode(next,quiet=false){
    stopPolling();cancelReplay();mode=next==="live"?"live":"demo";latestError=null;
    api.setSettings({mode});$("modeDemoBtn").classList.toggle("selected",mode==="demo");$("modeLiveBtn").classList.toggle("selected",mode==="live");
    if(mode==="demo"){
      status={...demo.status};dataPoints=[...demo.points];setConnection("is-demo","Data Rekaman");renderAll();renderModeNotice();
    }else{
      status={pressure_mmHg:null,target_mmHg:null,phase:null,test_mode:null,pump_on:null,valve_open:null,sensor_ok:null,leak_rate_mmHg_min:null,test_result:null,adc_raw:null,sensor_voltage:null};
      dataPoints=[];lastLiveSampleSecond=-1;liveStart=Date.now();
      setConnection("is-error","ESP32 · Menghubungkan");renderAll();renderModeNotice();pollLive();pollHandle=setInterval(pollLive,config.pollIntervalMs);
    }
    if(!quiet)showToast(mode==="demo"?"Data rekaman pengujian kelompok (bukan live).":"Koneksi live: mencoba mengambil data dari ESP32.");
  }
  function replayDemo(){if(mode!=="demo")return;if(replayTimer){cancelReplay();dataPoints=[...demo.points];renderAll();return;}
    const rows=[...demo.points];let i=1;$("replayBtn").innerHTML='<svg class="ico"><use href="#i-pause"/></svg> Hentikan pemutaran';
    replayTimer=setInterval(()=>{
      i++;dataPoints=rows.slice(0,i);chart.setData(dataPoints,"demo");setText("chartSamplesCount",`${dataPoints.length} / ${rows.length} titik rekaman`);
      if(i>=rows.length){cancelReplay();dataPoints=rows;renderAll();showToast("Pemutaran rekaman selesai. Ini bukan telemetry live.");}
    },180);
  }
  function downloadCSV(){
    if(!dataPoints.length){showToast("Belum ada sampel yang bisa diekspor.");return;}
    const csv=["fase,detik,T_mmHg",...dataPoints.map(p=>[p.phase,p.seconds.toFixed(3),p.pressure.toFixed(3)].join(","))].join("\r\n");
    const blob=new Blob(["\ufeff"+csv],{type:"text/csv;charset=utf-8;"});const url=URL.createObjectURL(blob);const a=document.createElement("a");a.href=url;a.download=mode==="demo"?"PRESCUE_cuplikan_uji_kelompok.csv":"PRESCUE_telemetry_live.csv";document.body.appendChild(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(url),1000);
    showToast(mode==="demo"?"Cuplikan data diekspor (bukan 300 sampel lengkap).":"Sampel sesi live diekspor.");
  }
  function updateNav(){const hashes=["overview","monitoring","testing","features","history","integration"];const target=location.hash.slice(1);const active=hashes.includes(target)?target:"overview";const labels={overview:"Ringkasan",monitoring:"Live Monitoring",testing:"Pengujian",features:"Mode & Daya",history:"Hasil & Riwayat",integration:"Integrasi API"};document.querySelectorAll("[data-nav]").forEach(el=>el.classList.toggle("active",el.dataset.nav===active));setText("breadcrumbTitle",labels[active]);$("sidebar").classList.remove("open");}
  function initialize(){
    const settings=api.getSettings();$("apiBaseUrl").value=settings.baseUrl;$("apiStatusPath").value=settings.statusPath;$("apiSamplesPath").value=settings.samplesPath;
    const now=new Date();$("todayLabel").textContent=now.toLocaleDateString("id-ID",{day:"numeric",month:"short",year:"numeric"});
    $("modeDemoBtn").addEventListener("click",()=>setMode("demo"));$("modeLiveBtn").addEventListener("click",()=>setMode("live"));
    $("replayBtn").addEventListener("click",replayDemo);$("exportCsvBtn").addEventListener("click",downloadCSV);
    $("quickApiSettings").addEventListener("click",()=>{location.hash="#integration";$("apiBaseUrl").focus();});
    $("menuToggle").addEventListener("click",()=>$("sidebar").classList.toggle("open"));
    document.addEventListener("keydown",e=>{if(e.key==="Escape")$("sidebar").classList.remove("open");});
    window.addEventListener("hashchange",updateNav);updateNav();
    $("apiForm").addEventListener("submit",e=>{
      e.preventDefault();const base=$("apiBaseUrl").value.trim();
      try{const url=new URL(base);if(!["http:","https:"].includes(url.protocol))throw new Error();}
      catch(_){setFeedback("Base URL harus http:// atau https://",false);return;}
      const statusPath=$("apiStatusPath").value.trim();const samplesPath=$("apiSamplesPath").value.trim();if(!statusPath.startsWith("/")){setFeedback("Endpoint status harus dimulai dengan /",false);return;}
      if(samplesPath&&!samplesPath.startsWith("/")){setFeedback("Endpoint sampel harus dimulai dengan / atau dikosongkan",false);return;}
      api.setSettings({baseUrl:base,statusPath,samplesPath});setFeedback("Pengaturan API disimpan di browser ini.",true);setText("endpointPreview",statusPath);showToast("Pengaturan tersimpan. Gunakan mode Live untuk polling.");if(mode==="live")setMode("live",true);
    });
    $("testApiBtn").addEventListener("click",async()=>{
      // Test button also saves input so the user can test directly, but does not switch modes.
      const base=$("apiBaseUrl").value.trim(),statusPath=$("apiStatusPath").value.trim();
      if(!/^https?:\/\//i.test(base)||!statusPath.startsWith("/")){setFeedback("Periksa base URL dan endpoint status.",false);return;}
      api.setSettings({baseUrl:base,statusPath,samplesPath:$("apiSamplesPath").value.trim()});setFeedback("Memeriksa respons JSON...",true);
      try{const result=await api.getLiveStatus();setFeedback(Number.isFinite(result.pressure_mmHg)?`ESP32 terhubung. Tekanan ${fnum(result.pressure_mmHg,3)} mmHg (belum diverifikasi).`:"ESP32 terhubung. Sensor belum siap, tekanan belum tersedia.",true);}
      catch(e){setFeedback(`Gagal: ${e.message}`,false);}
    });
    setMode(settings.mode,true);
  }
  function setFeedback(message,ok){const el=$("apiFeedback");el.textContent=message;el.className=`form-feedback ${ok?"ok":"error"}`;}
  initialize();
})();
