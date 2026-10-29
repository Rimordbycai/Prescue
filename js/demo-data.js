/* Data pilihan yang DISALIN dari log pengguna. Ini cuplikan, bukan 300 sampel utuh. */
(function(){
  const rows = [
    ["FILL",2.004,67.334],["FILL",4.011,103.658],["FILL",6.018,127.740],
    ["FILL",8.025,144.332],["FILL",8.781,150.044],
    ["SETTLE",0.206,150.471],["SETTLE",2.015,148.788],["SETTLE",5.030,146.655],
    ["SETTLE",8.045,145.387],["SETTLE",11.060,144.213],["SETTLE",15.080,142.827],
    ["SETTLE",18.899,141.547],["SETTLE",21.914,141.049],["SETTLE",25.934,140.409],
    ["SETTLE",29.954,139.627],
    ["RECORD",0.006,139.437],["RECORD",5.031,138.714],["RECORD",10.056,138.276],
    ["RECORD",15.081,137.280],["RECORD",20.106,136.712],["RECORD",25.131,135.953],
    ["RECORD",30.156,135.420],["RECORD",35.181,134.460],["RECORD",40.005,134.401],
    ["RECORD",45.030,134.009],["RECORD",50.055,133.156],["RECORD",55.080,132.469],
    ["RECORD",55.482,133.689],["RECORD",55.683,130.075],["RECORD",56.085,129.980],
    ["RECORD",57.090,129.790],["RECORD",58.095,129.601],["RECORD",59.100,129.340],
    ["RECORD",60.105,129.648]
  ];
  // Waktu pada log di-reset pada setiap fase. Sambung menjadi satu sumbu waktu.
  const phaseOffset = {FILL:0,SETTLE:8.781+2,RECORD:8.781+2+30};
  const points = rows.map(([phase,seconds,pressure])=>({
    phase,seconds,pressure,elapsed:phaseOffset[phase]+seconds
  }));
  window.PRESCUE_DEMO = Object.freeze({
    source:"cuplikan output serial kelompok",
    status:{pressure_mmHg:129.648,target_mmHg:150,phase:"RECORD",test_mode:"L",
      pump_on:false,valve_open:false,sensor_ok:true,leak_rate_mmHg_min:8.5736,
      test_result:"GAGAL",adc_raw:null,sensor_voltage:null,sample_count:300},
    details:{fill_duration_seconds:8.781,fill_final_pressure:150.044,
      settle_seconds:30,record_seconds:60,initial_record_mmHg:139.437,
      final_record_mmHg:129.648,leak_rate_mmHg_min:8.5736,reported_sample_count:300},
    points,
    history:[{name:"Uji kebocoran (L:150)",target:150,phase:"FILL → SETTLE → RECORD",duration:60,
      result:"GAGAL",leak:8.5736,date:null}],
    note:"Cuplikan data pilihan. Perhitungan 8.5736 mmHg/menit adalah hasil regresi firmware dari rekaman utuh, bukan dari titik yang ditampilkan grafik."
  });
})();
