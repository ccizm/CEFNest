// The popup is an ordinary extension page: the shim gives it chrome.storage
// and chrome.tabs, and the host's handlers answer both. It never touches the
// page directly — the one page it can name (the first non-extension tab) is
// reached through those two APIs, and the switch it writes is read by the
// content script on that page's next load.
(function () {
  "use strict";

  var DISABLED_KEY = "adblock.disabled";
  var RULES_KEY = "adblock.rules";
  var STATS_PREFIX = "adblock.stats.";
  var storage = chrome.storage.local;
  var site = "";
  var tab = null;

  function $(id) { return document.getElementById(id); }

  function siteKey(url) {
    try {
      var parsed = new URL(url);
      return parsed.hostname || parsed.protocol;
    } catch (e) {
      return "";
    }
  }

  function siteLabel(url) {
    try {
      var parsed = new URL(url);
      if (parsed.hostname) return parsed.hostname;
      var leaf = parsed.pathname.split("/").filter(Boolean).pop();
      return leaf ? leaf + " (local)" : parsed.protocol;
    } catch (e) {
      return url;
    }
  }

  function show(message) {
    $("status").textContent = message || "";
  }

  function fail(error, what) {
    show(what + ": " + (error && error.message ? error.message : "unknown"));
  }

  function renderRules(rules) {
    var list = $("rule-list");
    list.textContent = "";
    $("no-rules").hidden = rules.length > 0;

    rules.forEach(function (selector) {
      var row = document.createElement("li");
      var code = document.createElement("code");
      var remove = document.createElement("button");

      code.textContent = selector;
      code.title = selector;
      remove.type = "button";
      remove.textContent = "Remove";
      remove.addEventListener("click", function () { removeRule(selector); });

      row.appendChild(code);
      row.appendChild(remove);
      list.appendChild(row);
    });
  }

  function removeRule(selector) {
    if (!site) return;

    storage.get(RULES_KEY).then(function (stored) {
      var bySite = stored[RULES_KEY] || {};
      var rules = Object.prototype.toString.call(bySite[site]) === "[object Array]"
        ? bySite[site].slice()
        : [];
      var index = rules.indexOf(selector);

      if (index !== -1) rules.splice(index, 1);
      if (rules.length) bySite[site] = rules;
      else delete bySite[site];

      var items = {};
      items[RULES_KEY] = bySite;
      return storage.set(items);
    }).then(function () {
      show("Removed. The page stops hiding it on the next load.");
      load();
    }).catch(function (error) {
      fail(error, "Could not remove the rule");
    });
  }

  function load() {
    storage.get([DISABLED_KEY, RULES_KEY, STATS_PREFIX + site]).then(function (stored) {
      var disabled = stored[DISABLED_KEY] || {};
      var bySite = stored[RULES_KEY] || {};
      var stats = stored[STATS_PREFIX + site] || {};
      var rules = Object.prototype.toString.call(bySite[site]) === "[object Array]"
        ? bySite[site]
        : [];

      $("enabled").checked = !disabled[site];
      $("hidden").textContent = stats.hidden || 0;
      $("popups").textContent = stats.popups || 0;
      renderRules(rules);
      show("");
    }).catch(function (error) {
      $("enabled").disabled = true;
      $("reload").disabled = true;
      fail(error, "storage denied");
    });
  }

  chrome.tabs.query({}).then(function (tabs) {
    // The popup is a browser of its own; it is in this list too. The page the
    // person asked about is the active non-extension tab, or the only one.
    var pages = (tabs || []).filter(function (candidate) {
      return candidate.url &&
        candidate.url.indexOf("cefnest-extension://") !== 0;
    });

    tab = pages.filter(function (candidate) { return candidate.active; })[0] ||
      pages[0] || null;

    if (!tab) {
      $("site").textContent = "no page";
      $("enabled").disabled = true;
      $("reload").disabled = true;
      show("Open a page, then open Ad Sift again.");
      return;
    }

    site = siteKey(tab.url);
    $("site").textContent = siteLabel(tab.url);
    load();
  }, function (error) {
    $("enabled").disabled = true;
    $("reload").disabled = true;
    fail(error, "tabs.query failed");
  });

  $("enabled").addEventListener("change", function () {
    if (!site) return;

    storage.get(DISABLED_KEY).then(function (stored) {
      var disabled = stored[DISABLED_KEY] || {};

      if ($("enabled").checked) delete disabled[site];
      else disabled[site] = true;

      var items = {};
      items[DISABLED_KEY] = disabled;
      return storage.set(items);
    }).then(function () {
      show($("enabled").checked
        ? "Enabled on this site. Reload to apply."
        : "Disabled on this site. Reload to apply.");
    }).catch(function (error) {
      fail(error, "Could not write the switch");
      load();
    });
  });

  $("reload").addEventListener("click", function () {
    if (!tab) return;

    chrome.tabs.update(tab.id, { url: tab.url }).then(function () {
      show("Reloading…");
    }, function (error) {
      fail(error, "Reload failed");
    });
  });
})();
