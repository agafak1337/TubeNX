// ?frame=<id> оставляет на странице один фрейм в левом верхнем углу — для экспорта PNG.
// ?scale=1.5 масштабирует его (проверка 1920×1080).
(function () {
  var p = new URLSearchParams(location.search);
  var id = p.get("frame");
  if (!id) return;
  document.addEventListener("DOMContentLoaded", function () {
    var el = document.getElementById(id);
    if (!el) return;
    document.body.classList.add("solo");
    for (var n = el; n && n !== document.body; n = n.parentElement) n.classList.add("solo-keep");
    var s = parseFloat(p.get("scale") || "1");
    if (s !== 1) {
      el.style.transformOrigin = "0 0";
      el.style.transform = "scale(" + s + ")";
    }
  });
})();
