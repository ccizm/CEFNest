(function () {
  window.__nopermRan = 1;
  chrome.storage.local.get("x").then(function () {
    document.title = "NOPERM-OK";
  }, function (e) {
    document.title = "NOPERM-DENIED";
  });
})();
