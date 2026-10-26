// Ad Sift — the content-script engine.
//
// Everything this runtime lets a blocker do happens here: hide elements at
// document_start, keep a per-site switch, remember hand-picked selectors and
// count what was hidden. It cannot stop a request — there is no webRequest in
// this runtime, and no isolated world to hide in (docs/plugins.md) — so the
// list is cosmetic in the EasyList sense, and a hidden ad's bytes may already
// have been fetched.
//
// The engine runs in the top frame only. Subframes receive the shim and
// document_start scripts too, but a chrome.* reply is delivered to the main
// frame (docs/plugins.md), so a storage read from a subframe would wait
// forever. The parent still hides ad iframes through the list's iframe rules,
// which is what a cosmetic blocker can do from outside the frame.
(function () {
  "use strict";

  if (window.top !== window) return;
  if (window.__cefnestAdSift) return;

  var BASE_STYLE_ID = "cefnest-ad-sift-style";
  var CUSTOM_STYLE_ID = "cefnest-ad-sift-custom";
  var DISABLED_KEY = "adblock.disabled";
  var RULES_KEY = "adblock.rules";
  var STATS_PREFIX = "adblock.stats.";
  // A popup opened inside this window after a real gesture is the page's own
  // business. One opened from a timer has no gesture behind it, and that is
  // the shape an ad popunder uses.
  var POPUP_GRACE_MS = 3000;

  var SITE = siteKey(location.href);
  var baseSelectors = usable(parseList(window.__cefnestAdSiftRules || [], SITE));
  var customSelectors = [];
  var siteEnabled = true;
  // The shim defines window.chrome before this file runs. If another plugin's
  // shim got there first (no isolated world — the last one does not win, the
  // first window.chrome definition does), storage calls are judged against
  // that plugin's id and this one falls back to the static list.
  var chromeStore = window.chrome && window.chrome.storage
    ? window.chrome.storage.local
    : null;
  var hiddenCount = 0;
  var popupCount = 0;
  var lastSavedHidden = -1;
  var lastSavedPopups = -1;
  var statsTimer = 0;
  var recountTimer = 0;
  var observer = null;
  var applyScheduled = false;
  var pickerInstalled = false;
  var popupGuardInstalled = false;

  // A tiny surface for the page, and for the demo page's own check: it can
  // read what this blocker did without reaching into its storage.
  window.__cefnestAdSift = {
    site: SITE,
    enabled: function () { return siteEnabled; },
    selectors: function () { return baseSelectors.concat(customSelectors); },
    hidden: function () { return hiddenCount; },
    popups: function () { return popupCount; },
    recount: function () { recount(); return hiddenCount; }
  };

  // ---------------------------------------------------------------- list --

  function siteKey(url) {
    try {
      var parsed = new URL(url);
      return parsed.hostname || parsed.protocol;
    } catch (e) {
      return "";
    }
  }

  // EasyList's domain half: a rule scoped to example.com applies to the
  // domain and every subdomain. The site key for a file: document is
  // "file:", so only unscoped rules reach the local demo page.
  function domainMatches(domain, site) {
    if (!domain) return true;
    return site === domain ||
      (site.length > domain.length &&
       site.slice(-(domain.length + 1)) === "." + domain);
  }

  function parseList(rules, site) {
    var merged = [];
    var exceptions = [];

    for (var i = 0; i < rules.length; i++) {
      var rule = String(rules[i] || "");
      var scope;
      var selector;
      var at;

      if ((at = rule.indexOf("#@#")) !== -1) {
        scope = rule.slice(0, at);
        selector = rule.slice(at + 3);
        if (selector && domainMatches(scope, site)) exceptions.push(selector);
        continue;
      }

      if ((at = rule.indexOf("##")) === -1) continue;
      scope = rule.slice(0, at);
      selector = rule.slice(at + 2);
      if (!selector || (scope && !domainMatches(scope, site))) continue;
      if (merged.indexOf(selector) === -1) merged.push(selector);
    }

    return merged.filter(function (selector) {
      return exceptions.indexOf(selector) === -1;
    });
  }

  // One selector Chromium cannot parse is one rule lost, never the whole list:
  // the query is the syntax check, and the empty result at document_start is
  // exactly what a valid rule looks like before the parser makes any nodes.
  function usable(selectors) {
    var out = [];
    for (var i = 0; i < selectors.length; i++) {
      try {
        document.querySelector(selectors[i]);
        out.push(selectors[i]);
      } catch (e) {
        /* skip the rule */
      }
    }
    return out;
  }

  function cssFor(selectors) {
    var css = "";
    for (var i = 0; i < selectors.length; i++)
      css += selectors[i] + "{display:none !important}\n";
    return css;
  }

  // ------------------------------------------------------------- applying --

  // The style has to exist before the parser reaches the first ad element, so
  // it is appended as soon as documentElement is there. If a context is ever
  // created with no root yet, this retries at DOMContentLoaded rather than
  // giving up; the hiding then starts late, which is the honest worst case.
  function styleFor(id) {
    var style = document.getElementById(id);
    if (style) return style;

    var parent = document.head || document.documentElement;
    if (!parent) return null;

    style = document.createElement("style");
    style.id = id;
    style.setAttribute("data-cefnest-ad-sift", "");
    parent.appendChild(style);
    return style;
  }

  function apply() {
    if (!siteEnabled) return;

    var base = styleFor(BASE_STYLE_ID);
    var custom = styleFor(CUSTOM_STYLE_ID);
    if (base) base.textContent = cssFor(baseSelectors);
    if (custom) custom.textContent = cssFor(customSelectors);

    if (!base || !custom) {
      if (!applyScheduled) {
        applyScheduled = true;
        document.addEventListener("DOMContentLoaded", function () {
          applyScheduled = false;
          apply();
        }, { once: true });
      }
      return;
    }

    startCounting();
    recountSoon();
  }

  function removeStyle(id) {
    var style = document.getElementById(id);
    if (style) style.remove();
  }

  function offForSite() {
    siteEnabled = false;
    removeStyle(BASE_STYLE_ID);
    removeStyle(CUSTOM_STYLE_ID);
    if (observer) {
      observer.disconnect();
      observer = null;
    }
    if (recountTimer) {
      clearTimeout(recountTimer);
      recountTimer = 0;
    }
  }

  // --------------------------------------------------------------- counts --

  function collect(seen, selector) {
    var found;
    try {
      found = document.querySelectorAll(selector);
    } catch (e) {
      return;
    }
    for (var i = 0; i < found.length; i++) seen.add(found[i]);
  }

  function recount() {
    var seen = new Set();
    for (var i = 0; i < baseSelectors.length; i++)
      collect(seen, baseSelectors[i]);
    for (var j = 0; j < customSelectors.length; j++)
      collect(seen, customSelectors[j]);

    hiddenCount = seen.size;
    if (hiddenCount !== lastSavedHidden || popupCount !== lastSavedPopups)
      saveStats();
  }

  function recountSoon() {
    if (recountTimer) clearTimeout(recountTimer);
    recountTimer = setTimeout(function () {
      recountTimer = 0;
      recount();
    }, 300);
  }

  function startCounting() {
    if (observer || (!baseSelectors.length && !customSelectors.length))
      return;
    if (!document.documentElement) return;

    observer = new MutationObserver(function () {
      recountSoon();
    });
    observer.observe(document.documentElement, { childList: true, subtree: true });
    window.addEventListener("load", recount, true);
  }

  // The popup reads this key when it opens. One key per site keeps two tabs on
  // different sites from overwriting each other; two tabs on the same site
  // still race, and the last writer is the answer.
  // A pending timer is not re-armed: recount() is also the page's read path
  // (the demo bar calls it every 900 ms), and re-arming on each read would
  // push the write past every interval and never let it run. The write that
  // does run reads the counts as they are when it fires.
  function saveStats() {
    if (!chromeStore || !siteEnabled || statsTimer) return;

    statsTimer = setTimeout(function () {
      statsTimer = 0;
      if (!chromeStore || !siteEnabled) return;

      var items = {};
      items[STATS_PREFIX + SITE] = { hidden: hiddenCount, popups: popupCount };
      lastSavedHidden = hiddenCount;
      lastSavedPopups = popupCount;
      chromeStore.set(items).catch(function () {});
    }, 600);
  }

  // --------------------------------------------------------------- picker --

  // Alt+Shift+Click instead of a context-menu row. The Web frontend's menu
  // bridge has no per-page cleanup: a row created by a content script outlives
  // the page that owns its callback and its slot is never returned, so a
  // blocker that runs on every page would exhaust the 0-199 space. The
  // modifier click needs no bridge and dies with the page.
  function selectorFor(element) {
    if (!element || element.nodeType !== 1 || element === document.documentElement)
      return null;

    var id = element.getAttribute("id");
    if (id && /^[A-Za-z][A-Za-z0-9_-]*$/.test(id)) {
      var byId = "#" + id;
      if (document.querySelectorAll(byId).length === 1) return byId;
    }

    var names = (element.getAttribute("class") || "").trim().split(/\s+/);
    var stable = [];
    for (var i = 0; i < names.length && stable.length < 2; i++)
      if (/^[A-Za-z][A-Za-z0-9_-]*$/.test(names[i])) stable.push(names[i]);
    if (stable.length) {
      var byClass = element.localName + "." + stable.join(".");
      if (document.querySelectorAll(byClass).length === 1) return byClass;
    }

    // Last resort: a path of tag names with :nth-of-type() only where a parent
    // has more than one child of that tag, stopping at the first prefix that
    // names exactly one element.
    var path = [];
    var node = element;
    while (node && node.nodeType === 1 && node !== document.documentElement) {
      var part = node.localName;
      var parent = node.parentElement;

      if (parent) {
        var index = 1;
        var sameTag = 0;
        for (var j = 0; j < parent.children.length; j++) {
          if (parent.children[j].localName !== node.localName) continue;
          sameTag += 1;
          if (parent.children[j] === node) index = sameTag;
        }
        if (sameTag > 1) part += ":nth-of-type(" + index + ")";
      }

      path.unshift(part);
      var candidate = path.join(" > ");
      if (document.querySelectorAll(candidate).length === 1) return candidate;
      node = parent;
    }
    return path.length ? path.join(" > ") : null;
  }

  function shortSelector(selector) {
    return selector.length > 44 ? selector.slice(0, 41) + "..." : selector;
  }

  function rememberRule(selector) {
    if (!chromeStore) return;

    chromeStore.get(RULES_KEY).then(function (stored) {
      var bySite = stored[RULES_KEY] || {};
      var list = Object.prototype.toString.call(bySite[SITE]) === "[object Array]"
        ? bySite[SITE].slice()
        : [];

      if (list.indexOf(selector) === -1) list.push(selector);
      bySite[SITE] = list;

      var items = {};
      items[RULES_KEY] = bySite;
      return chromeStore.set(items);
    }).catch(function () {});
  }

  function installPicker() {
    if (pickerInstalled) return;
    pickerInstalled = true;

    window.addEventListener("click", function (event) {
      if (!siteEnabled || !event.altKey || !event.shiftKey) return;
      if (typeof event.button === "number" && event.button !== 0) return;
      if (!event.target || event.target.nodeType !== 1) return;

      var selector = selectorFor(event.target);
      if (!selector) return;

      event.preventDefault();
      event.stopPropagation();

      if (customSelectors.indexOf(selector) === -1) {
        customSelectors.push(selector);
        apply();
        rememberRule(selector);
      }
      flash("Ad Sift: hiding " + shortSelector(selector));
    }, true);
  }

  // ---------------------------------------------------------- popup guard --

  function installPopupGuard() {
    if (popupGuardInstalled) return;

    var lastGesture = 0;
    var gestureEvents = ["pointerdown", "mousedown", "touchstart", "keydown"];
    for (var i = 0; i < gestureEvents.length; i++)
      window.addEventListener(gestureEvents[i], function () {
        lastGesture = Date.now();
      }, true);

    var nativeOpen = window.open;
    if (typeof nativeOpen !== "function") return;

    try {
      window.open = function () {
        if (siteEnabled && Date.now() - lastGesture > POPUP_GRACE_MS) {
          popupCount += 1;
          saveStats();
          flash("Ad Sift: automatic popup blocked");
          return null;
        }
        return nativeOpen.apply(window, arguments);
      };
      popupGuardInstalled = true;
    } catch (e) {
      /* a page that froze window.open keeps this feature off, not the list */
    }
  }

  function flash(message) {
    var box = document.createElement("div");
    box.setAttribute("data-cefnest-ad-sift", "toast");
    box.style.cssText =
      "position:fixed;left:50%;bottom:22px;transform:translateX(-50%);" +
      "z-index:2147483647;max-width:80vw;padding:8px 14px;border-radius:8px;" +
      "background:#14202e;color:#e8eef8;font:13px/1.4 system-ui,sans-serif;" +
      "box-shadow:0 6px 20px rgba(0,0,0,.35);pointer-events:none;" +
      "white-space:nowrap;overflow:hidden;text-overflow:ellipsis;";
    box.textContent = message;
    (document.body || document.documentElement).appendChild(box);
    setTimeout(function () { box.remove(); }, 1600);
  }

  // ---------------------------------------------------------------- config --

  function onConfig(stored) {
    var disabled = stored[DISABLED_KEY] || {};
    if (disabled[SITE]) {
      offForSite();
      return;
    }

    var bySite = stored[RULES_KEY] || {};
    if (Object.prototype.toString.call(bySite[SITE]) === "[object Array]")
      customSelectors = usable(bySite[SITE].map(String));

    apply();
    installPicker();
    installPopupGuard();
    startCounting();
    recount();
  }

  function onConfigFailed() {
    // No storage is not no blocker: the static list still hides, and the
    // picker works for this page but cannot remember a rule for the next one.
    apply();
    installPicker();
    installPopupGuard();
    startCounting();
    recount();
  }

  // The static list goes in before the first storage round trip: an ad that
  // needs a disk read to disappear has already been painted. The per-site
  // switch then lands a few milliseconds later, so a disabled site flashes its
  // ads for that one round trip.
  apply();

  if (chromeStore) {
    try {
      chromeStore.get([DISABLED_KEY, RULES_KEY])
        .then(onConfig, onConfigFailed)
        .catch(function () {});
    } catch (e) {
      onConfigFailed();
    }
  } else {
    onConfigFailed();
  }
})();
