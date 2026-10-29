/* Grafik Canvas tanpa library eksternal agar mudah di-host di LittleFS ESP32. */
(function(){
  const color={FILL:"#438ef1",SETTLE:"#e9a84f",RECORD:"#1fb49b",LIVE:"#438ef1"};
  function create(canvas,tooltip){
    const ctx=canvas.getContext("2d");
    let points=[],lastBox=null,hoverPoint=-1,overlayMode="demo";
    const pad={top:26,right:20,bottom:40,left:51};
    function render(){
      if(!ctx)return;
      const rect=canvas.getBoundingClientRect();const w=Math.max(200,rect.width),h=Math.max(180,rect.height);
      const dpr=Math.min(window.devicePixelRatio||1,2);
      canvas.width=Math.round(w*dpr);canvas.height=Math.round(h*dpr);
      ctx.setTransform(dpr,0,0,dpr,0,0);
      const innerW=w-pad.left-pad.right,innerH=h-pad.top-pad.bottom;
      const maxT=Math.max(60,Math.ceil((Math.max(0,...points.map(x=>x.elapsed))+2)/10)*10);
      const maxP=Math.max(180,Math.ceil((Math.max(0,...points.map(x=>x.pressure))+12)/30)*30);
      const xx=t=>pad.left+(t/maxT)*innerW;
      const yy=p=>pad.top+innerH-(p/maxP)*innerH;
      lastBox={w,h,maxT,maxP,xx,yy,pad};
      ctx.clearRect(0,0,w,h);
      if(points.length && overlayMode==="demo"){
        const ranges=[{name:"FILL",lo:0,hi:8.781,col:"rgba(67,142,241,.09)"},{name:"SETTLE",lo:8.781,hi:40.781,col:"rgba(233,168,79,.095)"},{name:"RECORD",lo:40.781,hi:maxT,col:"rgba(31,180,155,.065)"}];
        for(const r of ranges){ctx.fillStyle=r.col;ctx.fillRect(xx(r.lo),pad.top,Math.max(0,xx(r.hi)-xx(r.lo)),innerH);ctx.fillStyle="#829bb7";ctx.font="700 10px system-ui";const width=ctx.measureText(r.name).width;if(xx(r.hi)-xx(r.lo)>width+12)ctx.fillText(r.name,Math.min(xx(r.hi)-width-4,xx(r.lo)+7),pad.top+15);}
      }
      ctx.font="10px system-ui";ctx.fillStyle="#9cabbe";ctx.strokeStyle="#eaf0f8";ctx.lineWidth=1;
      for(let i=0;i<=4;i++){const p=i/4*maxP,y=yy(p);ctx.beginPath();ctx.moveTo(pad.left,y);ctx.lineTo(w-pad.right,y);ctx.stroke();ctx.textAlign="right";ctx.fillText(String(Math.round(p)),pad.left-10,y+3);}
      const count= w<430?4:5;
      for(let i=0;i<=count;i++){const t=i/count*maxT,x=xx(t);ctx.beginPath();ctx.moveTo(x,pad.top);ctx.lineTo(x,h-pad.bottom);ctx.stroke();ctx.textAlign="center";ctx.fillText(String(Math.round(t)),x,h-pad.bottom+18);}
      ctx.save();ctx.fillStyle="#8399b2";ctx.font="10.5px system-ui";ctx.textAlign="center";ctx.fillText("Waktu (detik)",pad.left+innerW/2,h-5);ctx.translate(14,pad.top+innerH/2);ctx.rotate(-Math.PI/2);ctx.fillText("Tekanan (mmHg)",0,0);ctx.restore();
      if(points.length===0){ctx.fillStyle="#93a4b8";ctx.font="12px system-ui";ctx.textAlign="center";ctx.fillText("Belum ada data sensor dari ESP32",pad.left+innerW/2,pad.top+innerH/2);return;}
      // A common line joins observations; phase colors only describe each known segment.
      for(let i=1;i<points.length;i++){
        ctx.beginPath();ctx.moveTo(xx(points[i-1].elapsed),yy(points[i-1].pressure));ctx.lineTo(xx(points[i].elapsed),yy(points[i].pressure));ctx.strokeStyle=color[points[i].phase]||color.LIVE;ctx.lineWidth=2.6;ctx.lineJoin="round";ctx.lineCap="round";ctx.stroke();
      }
      if(points.length===1){const p=points[0];ctx.beginPath();ctx.arc(xx(p.elapsed),yy(p.pressure),3.4,0,2*Math.PI);ctx.fillStyle=color[p.phase]||color.LIVE;ctx.fill();}
      const idx=hoverPoint>=0&&hoverPoint<points.length?hoverPoint:points.length-1;
      const p=points[idx];ctx.beginPath();ctx.arc(xx(p.elapsed),yy(p.pressure),5,0,Math.PI*2);ctx.fillStyle="#fff";ctx.fill();ctx.lineWidth=2.5;ctx.strokeStyle=color[p.phase]||color.LIVE;ctx.stroke();
    }
    function nearest(event){
      if(!lastBox||!points.length)return;
      const rect=canvas.getBoundingClientRect(), x=event.clientX-rect.left;
      const t=(x-lastBox.pad.left)/(lastBox.w-lastBox.pad.left-lastBox.pad.right)*lastBox.maxT;
      let best=0,diff=Infinity;points.forEach((p,i)=>{const d=Math.abs(p.elapsed-t);if(d<diff){diff=d;best=i;}});
      hoverPoint=best;render();const p=points[best];tooltip.hidden=false;
      tooltip.textContent=`${p.phase}  ·  ${p.seconds.toFixed(2)} s  ·  ${p.pressure.toFixed(3)} mmHg`;
      const left=Math.max(6,Math.min(rect.width-tooltip.offsetWidth-6,lastBox.xx(p.elapsed)+10));
      const top=Math.max(6,Math.min(rect.height-tooltip.offsetHeight-6,lastBox.yy(p.pressure)-48));
      tooltip.style.left=`${left}px`;tooltip.style.top=`${top}px`;
    }
    canvas.addEventListener("mousemove",nearest);
    canvas.addEventListener("mouseleave",()=>{hoverPoint=-1;tooltip.hidden=true;render();});
    window.addEventListener("resize",render);
    if(typeof ResizeObserver!=="undefined")new ResizeObserver(render).observe(canvas.parentElement);
    return {setData(value,mode="demo"){points=Array.isArray(value)?value.filter(p=>Number.isFinite(p.pressure)&&Number.isFinite(p.elapsed)):[];overlayMode=mode;hoverPoint=-1;tooltip.hidden=true;render();},getPoints:()=>[...points],render};
  }
  window.PRESCUE_CHART={create};
})();
