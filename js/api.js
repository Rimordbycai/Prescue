/* PRESCUE API Adapter
 * Ini frontend data-access layer, BUKAN backend ESP32. HTTP server dibuat teman firmware.
 * Ubah field mapping di normalizeStatus bila nama field ESP32 berbeda.
 */
(function(){
  const cfg=window.PRESCUE_CONFIG;
  const defaultSettings={mode:cfg.defaultMode,baseUrl:cfg.defaultBaseUrl,statusPath:cfg.defaultStatusPath,samplesPath:cfg.defaultSamplesPath};
  function safeRead(){
    try {let saved=JSON.parse(localStorage.getItem("prescue-api-settings-v1")||"null");return saved&&typeof saved==="object"?{...defaultSettings,...saved}:({...defaultSettings});}
    catch(_){return {...defaultSettings};}
  }
  let settings=safeRead();
  function setSettings(patch){settings={...settings,...patch};try{localStorage.setItem("prescue-api-settings-v1",JSON.stringify(settings));}catch(_){/* private browser mode */}return {...settings};}
  const getSettings=()=>({...settings});
  function buildUrl(path){
    const base=settings.baseUrl.replace(/\/+$/g,"");
    if(!base && !path.startsWith("/"))return path;
    return `${base}${path.startsWith("/")?path:`/${path}`}`;
  }
  async function requestJSON(path){
    const controller=new AbortController();
    const timeout=setTimeout(()=>controller.abort(),cfg.requestTimeoutMs);
    try{
      const response=await fetch(buildUrl(path),{method:"GET",headers:{Accept:"application/json"},signal:controller.signal,cache:"no-store"});
      if(!response.ok)throw new Error(`HTTP ${response.status} (${response.statusText||"gagal"})`);
      const data=await response.json();
      if(data===null||typeof data!=="object")throw new Error("Respons bukan JSON objek/array");
      return data;
    }catch(err){if(err.name==="AbortError")throw new Error("Koneksi timeout. Cek IP dan Wi-Fi ESP32.");throw err;}
    finally{clearTimeout(timeout);}
  }
  function first(obj,keys){for(const key of keys){if(Object.prototype.hasOwnProperty.call(obj,key) && obj[key]!==null && obj[key]!==undefined)return obj[key];}return null;}
  function numberOrNull(x){if(x===null||x===undefined||x==="")return null;const n=Number(x);return Number.isFinite(n)?n:null;}
  function booleanOrNull(x){if(x===true||x===false)return x;if(x===1||x==="1"||x==="true")return true;if(x===0||x==="0"||x==="false")return false;return null;}
  function normalizeStatus(raw){
    const d=(raw && typeof raw.data==="object" && !Array.isArray(raw.data))?raw.data:raw;
    if(!d||typeof d!=="object"||Array.isArray(d))throw new Error("Endpoint status tidak mengembalikan objek JSON");
    const pressure=numberOrNull(first(d,["pressure_mmHg","pressure_mmhg","pressure","T_mmHg"]));
    if(pressure===null && !["pressure_mmHg","pressure_mmhg","pressure","T_mmHg"].some(k=>Object.prototype.hasOwnProperty.call(d,k)))throw new Error("Field tekanan tidak ditemukan. Sesuaikan mapping di js/api.js");
    // PRESCUE CONNECTED UI V6: jangan samakan simulator dengan ESP32 asli.
    return {
      connected:booleanOrNull(first(d,["connected","usb_connected"])),
      is_simulated:booleanOrNull(first(d,["is_simulated"])),
      remote_commands_enabled:booleanOrNull(first(d,["remote_commands_enabled"])),
      safety_ok:booleanOrNull(first(d,["safety_ok"])),
      busy:booleanOrNull(first(d,["busy"])),
      pressure_mmHg:pressure,
      target_mmHg:numberOrNull(first(d,["target_mmHg","target_pressure_mmHg","target_pressure","target"])),
      phase:first(d,["phase","fase","stage"])||null,
      test_mode:first(d,["test_mode","mode","test_code"])||null,
      pump_on:booleanOrNull(first(d,["pump_on","pump","motor_on"])),
      valve_open:booleanOrNull(first(d,["valve_open","solenoid_open","valve_on"])),
      sensor_ok:booleanOrNull(first(d,["sensor_ok","sensor_connected"])),
      leak_rate_mmHg_min:numberOrNull(first(d,["leak_rate_mmHg_min","leak_rate_mmhg_min","leak_rate","leak_mmHg_min"])),
      test_result:first(d,["test_result","result","hasil"])||null,
      adc_raw:numberOrNull(first(d,["adc_raw","raw_adc","mentah"])),
      sensor_voltage:numberOrNull(first(d,["sensor_voltage","sensor_voltage_v","voltage_v","vout"])),
      sample_count:numberOrNull(first(d,["sample_count","samples_count","n"])),
      elapsed_seconds:numberOrNull(first(d,["elapsed_seconds","elapsed_s","detik","seconds"])),
      // Jika ingin hemat endpoint, firmware bisa menyisipkan samples di objek status.
      samples:Array.isArray(d.samples)?d.samples:null
    };
  }
  function normalizeSamples(raw){
    const a=Array.isArray(raw)?raw:(Array.isArray(raw.samples)?raw.samples:(Array.isArray(raw.data)?raw.data:[]));
    return a.map((item,i)=>{
      const d=Array.isArray(item)?{seconds:item[0],pressure_mmHg:item[1],phase:item[2]}:item;
      const pressure=numberOrNull(first(d,["pressure_mmHg","pressure_mmhg","pressure","T_mmHg"]));
      const seconds=numberOrNull(first(d,["elapsed_seconds","elapsed","seconds","time_s","t","detik"]));
      if(pressure===null||seconds===null)return null;
      return {pressure,elapsed:seconds,seconds,phase:String(first(d,["phase","fase","stage"])||"LIVE")};
    }).filter(Boolean).sort((a,b)=>a.elapsed-b.elapsed).slice(-cfg.maxChartPoints);
  }
  async function getLiveStatus(){return normalizeStatus(await requestJSON(settings.statusPath));}
  async function getLiveSamples(){if(!settings.samplesPath.trim())return null;return normalizeSamples(await requestJSON(settings.samplesPath));}

  async function sendCommand(command){
    if(typeof command!=="string" || !/^(?:[GL]:\d{1,3}|O|X)$/.test(command))
      throw new Error("Perintah tidak dikenal");
    if(/^[GL]:/.test(command)){
      const target=Number(command.slice(2));
      if(!Number.isInteger(target)||target<20||target>150)
        throw new Error("Target harus 20–150 mmHg");
    }
    const controller=new AbortController();
    const timeout=setTimeout(()=>controller.abort(),10000);
    try{
      const response=await fetch(buildUrl("/api/command"),{
        method:"POST",headers:{"Content-Type":"application/json"},
        body:JSON.stringify({command}),signal:controller.signal,cache:"no-store"
      });
      const data=await response.json().catch(()=>({}));
      if(!response.ok)throw new Error(data.error||`HTTP ${response.status}`);
      return data;
    }catch(error){
      if(error.name==="AbortError")throw new Error("Perintah timeout; periksa kondisi alat sebelum mengirim ulang");
      throw error;
    }finally{clearTimeout(timeout);}
  }
  return void (window.PRESCUE_API={getSettings,setSettings,buildUrl,requestJSON,normalizeStatus,normalizeSamples,getLiveStatus,getLiveSamples,sendCommand});
})();
