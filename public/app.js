const $=s=>document.querySelector(s);
let me=null, authMode="login", currentVideo=null;

async function api(url,opts={}){const r=await fetch(url,{credentials:"same-origin",...opts});let d={};try{d=await r.json()}catch{}if(!r.ok)throw new Error(d.error||"Request failed");return d}
function modal(id,on=true){$( "#"+id).classList.toggle("show",on)}
function esc(s){return String(s??"").replace(/[&<>"']/g,m=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[m]))}

async function refreshMe(){
  try{me=await api("/api/me")}catch{me={loggedIn:false}}
  $("#loginBtn").textContent=me.loggedIn?`@${me.username}`:"Login";
}
async function loadVideos(q=""){
  $("#status").textContent="Loading...";
  const data=await api("/api/videos"+(q?"?q="+encodeURIComponent(q):""));
  $("#status").textContent=`${data.length} video${data.length===1?"":"s"}`;
  const grid=$("#grid");
  grid.innerHTML=data.length?data.map(v=>`
  <article class="videoCard" onclick="watch(${v.id})">
    ${v.thumb?`<img class="thumb" src="${esc(v.thumb)}" alt="">`:`<div class="thumbFallback">▶</div>`}
    <div class="vbody"><div class="title">${esc(v.title)}</div><div class="meta">${esc(v.creator)} · ${v.views} views · ${v.likes} likes</div></div>
  </article>`).join(""):`<div class="empty">No videos found.</div>`;
}
window.watch=async id=>{
  try{
    const v=await api("/api/video/"+id); currentVideo=v;
    $("#watchTitle").textContent=v.title;
    $("#watchMeta").textContent=`${v.creator} · ${v.views} views`;
    $("#watchDesc").textContent=v.description;
    $("#player").src="/media/"+encodeURIComponent(v.filename);
    await loadComments(id); modal("watchModal");
  }catch(e){alert(e.message)}
};
async function loadComments(id){
  const cs=await api(`/api/video/${id}/comments`);
  $("#commentList").innerHTML=cs.length?cs.map(c=>`<div class="comment"><b>${esc(c.username)}</b><span>${esc(c.body)}</span></div>`).join(""):"<p class='meta'>No comments yet.</p>";
}
function openAuth(){modal("authModal");$("#authError").textContent=""}
$("#loginBtn").onclick=()=>openAuth();
$("#uploadBtn").onclick=()=>me?.loggedIn?modal("uploadModal"):openAuth();
$("#heroUpload").onclick=()=>me?.loggedIn?modal("uploadModal"):openAuth();
document.querySelectorAll("[data-close]").forEach(b=>b.onclick=()=>modal(b.dataset.close,false));
$("#switchAuth").onclick=()=>{authMode=authMode==="login"?"register":"login";$("#authTitle").textContent=authMode==="login"?"Login":"Create account";$("#switchAuth").textContent=authMode==="login"?"Create account":"Back to login";};
$("#authForm").onsubmit=async e=>{
 e.preventDefault();$("#authError").textContent="";
 const fd=new FormData(e.target);
 try{
  await api("/api/"+authMode,{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:new URLSearchParams(fd)});
  modal("authModal",false);await refreshMe();
 }catch(err){$("#authError").textContent=err.message}
};
$("#search").onsubmit=e=>{e.preventDefault();const q=$("#q").value.trim();$("#feedTitle").textContent=q?`Results for “${q}”`:"Latest videos";loadVideos(q)};
$("#uploadForm").onsubmit=e=>{
 e.preventDefault(); if(!me?.loggedIn)return openAuth();
 const form=e.target, xhr=new XMLHttpRequest(), data=new FormData(form);
 $("#uploadMsg").textContent="Uploading...";$("#progressBar").style.width="0%";
 xhr.upload.onprogress=e=>{if(e.lengthComputable)$("#progressBar").style.width=(e.loaded/e.total*100)+"%"};
 xhr.onload=async()=>{try{const d=JSON.parse(xhr.responseText);if(xhr.status>=400)throw new Error(d.error);$("#uploadMsg").textContent="Upload complete.";form.reset();setTimeout(()=>modal("uploadModal",false),700);loadVideos()}catch(err){$("#uploadMsg").textContent=err.message}};
 xhr.onerror=()=>$("#uploadMsg").textContent="Network error.";
 xhr.open("POST","/api/upload");xhr.send(data);
};
$("#likeBtn").onclick=async()=>{
 if(!me?.loggedIn)return openAuth();
 try{const d=await api(`/api/video/${currentVideo.id}/like`,{method:"POST"});$("#likeBtn").textContent=d.liked?"♥ Liked":"♡ Like"}catch(e){alert(e.message)}
};
$("#commentForm").onsubmit=async e=>{
 e.preventDefault();if(!me?.loggedIn)return openAuth();
 try{await api(`/api/video/${currentVideo.id}/comments`,{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:new URLSearchParams(new FormData(e.target))});e.target.reset();loadComments(currentVideo.id)}catch(err){alert(err.message)}
};
(async()=>{await refreshMe();await loadVideos()})();
