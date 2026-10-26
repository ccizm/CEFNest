// Ad Sift's filter list, in EasyList's cosmetic shape:
//
//   ##selector             everywhere
//   example.com##selector  on example.com and its subdomains
//   example.com#@#selector take one rule back on that site
//
// Network filters ("||ads.example^") are not here because this runtime has no
// webRequest: a content script can hide a request's result, never the request
// itself (docs/plugins.md, "What a plugin can never do here"). The list is
// deliberately conservative — ad networks' own frame hosts and containers that
// only ads use — and the popup's picker is the escape hatch for everything
// else. "example.com#@#.ad-slot" would be the shape of an exception; none is
// listed because no rule below is broad enough to collide with real content.
(function () {
  window.__cefnestAdSiftRules = [
    // Ad network frames and slots.
    "##ins.adsbygoogle",
    "##[id^=\"google_ads\"]",
    "##[id^=\"div-gpt-ad\"]",
    "##[id^=\"dfp-ad-\"]",
    "##[data-ad-slot]",
    "##[data-ad-client]",
    "##iframe[src*=\"doubleclick.net\"]",
    "##iframe[src*=\"googlesyndication.com\"]",
    "##iframe[src*=\"adservice.google\"]",
    "##iframe[src*=\"adnxs.com\"]",
    "##iframe[src*=\"amazon-adsystem.com\"]",
    "##iframe[title=\"Advertisement\"]",

    // Containers named by their site, not by an ad network.
    "##.ad-banner",
    "##.ad-container",
    "##.ad-slot",
    "##.ad-unit",
    "##.ad-wrapper",
    "##.ad-placeholder",
    "##.ads-container",
    "##.ads-wrapper",
    "##.advertisement",
    "##[aria-label=\"Advertisement\"]",
    "##.gpt-ad",
    "##.dfp-ad",
    "##.banner-ad",
    "##.sidebar-ad",
    "##.sticky-ad",
    "##.inline-ad",
    "##.video-ads",
    "##.ad-overlay",
    "##.ad-popup",
    "##.popup-ad",

    // Sponsored content and the recommendation widgets that pretend not to be
    // ads. These are the names the widgets ship with, so they are safe to
    // match without a site scope.
    "##.sponsored-content",
    "##.sponsored-post",
    "##.native-ad",
    "##.promoted-content",
    "##.taboola",
    "##.outbrain",
    "##.mgid",
    "##.revcontent",
    "##.zergnet",

    // Site-scoped rules: the domain half of EasyList's syntax, and the shape a
    // longer list would use to keep a generic class from hiding real content
    // somewhere else.
    "youtube.com##.video-ads",
    "youtube.com##.ytp-ad-module",
    "youtube.com##.ytp-ad-overlay-container",
    "youtube.com##ytd-display-ad-renderer",
    "youtube.com##ytd-promoted-sparkles-web-renderer",
    "youtube.com##ytd-ad-slot-renderer",
    "youtube.com##ytd-in-feed-ad-layout-renderer",
    "reddit.com##shreddit-ad-post"
  ];
})();
