"use strict";

// The web client mock-up. Everything here draws sample states: no rules run, no
// server is called. Geometry is ported from the desktop client's src/table/ (the
// file each function follows is named beside it); the screens it has no
// counterpart for are drawn in its style.

// ---------- small DOM helpers ----------

const SVG_NS = "http://www.w3.org/2000/svg";

function h(tag, props, ...kids)
{
  const el = document.createElement(tag);
  for (const [key, value] of Object.entries(props ?? {}))
  {
    if (value === null || value === undefined || value === false)
      continue;
    if (key === "class")
      el.className = value;
    else if (key === "style")
      el.style.cssText = value;
    else if (key.startsWith("on"))
      el.addEventListener(key.slice(2), value);
    else
      el.setAttribute(key, value === true ? "" : value);
  }
  for (const kid of kids.flat(Infinity))
  {
    if (kid === null || kid === undefined || kid === false)
      continue;
    el.append(kid instanceof Node ? kid : String(kid));
  }
  return el;
}

const ICONS = {
  newGame: '<circle cx="12" cy="12" r="9"/><path d="M12 7v10M7 12h10"/>',
  settings: '<circle cx="12" cy="12" r="3.2"/><path d="M12 2.5v3M12 18.5v3M2.5 12h3M18.5 12h3M5.3 5.3l2.1 2.1M16.6 16.6l2.1 2.1M5.3 18.7l2.1-2.1M16.6 7.4l2.1-2.1"/>',
  rules: '<path d="M4 5c3-1.5 5.5-1.5 8 0v15c-2.5-1.5-5-1.5-8 0z"/><path d="M20 5c-3-1.5-5.5-1.5-8 0v15c2.5-1.5 5-1.5 8 0z"/>',
  exit: '<path d="M14 4H5v16h9"/><path d="M10 12h10M16 8l4 4-4 4"/>',
  history: '<path d="M4 4h16v16H4z"/><path d="M4 9h16M4 14h16M10 4v16"/>',
  last: '<rect x="3" y="6" width="10" height="14" rx="1.5"/><path d="M9 6V4.5A1.5 1.5 0 0 1 10.5 3h8A1.5 1.5 0 0 1 20 4.5v11a1.5 1.5 0 0 1-1.5 1.5H13"/>',
  chip: '<rect x="6" y="6" width="12" height="12" rx="2"/><rect x="9.5" y="9.5" width="5" height="5" rx="0.8"/><path d="M9.5 3v3M14.5 3v3M9.5 18v3M14.5 18v3M3 9.5h3M3 14.5h3M18 9.5h3M18 14.5h3"/>',
  link: '<path d="M10 14a4.5 4.5 0 0 0 6.4 0l3-3a4.5 4.5 0 0 0-6.4-6.4l-1.2 1.2"/><path d="M14 10a4.5 4.5 0 0 0-6.4 0l-3 3a4.5 4.5 0 0 0 6.4 6.4l1.2-1.2"/>',
  copy: '<rect x="8" y="8" width="12" height="12" rx="2"/><path d="M16 8V5a1 1 0 0 0-1-1H5a1 1 0 0 0-1 1v10a1 1 0 0 0 1 1h3"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  people: '<circle cx="9" cy="8" r="3.2"/><path d="M3 20c0-3.5 2.7-6 6-6s6 2.5 6 6"/><circle cx="17" cy="9" r="2.6"/><path d="M15.5 14.2c3 .2 5.5 2.4 5.5 5.8"/>',
  globe: '<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3c3 3.5 3 14.5 0 18M12 3c-3 3.5-3 14.5 0 18"/>',
  code: '<path d="M8 7l-5 5 5 5M16 7l5 5-5 5"/>',
  info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7.6v.4"/>',
  away: '<path d="M2 8.5a15 15 0 0 1 20 0M5.5 12a10 10 0 0 1 13 0M9 15.5a5 5 0 0 1 6 0"/><path d="M12 19h.01"/><path d="M3 3l18 18"/>',
  back: '<path d="M2 8.5a15 15 0 0 1 20 0M5.5 12a10 10 0 0 1 13 0M9 15.5a5 5 0 0 1 6 0"/><path d="M12 19h.01"/>',
  check: '<path d="M5 12.5l4.5 4.5L19 7.5"/>',
  hourglass: '<path d="M7 3h10M7 21h10M8 3c0 5 8 5 8 9s-8 4-8 9M16 3c0 5-8 5-8 9s8 4 8 9"/>',
  clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
  turn: '<rect x="7" y="2.5" width="10" height="17" rx="2"/><path d="M11 16.5h2"/><path d="M19.5 13a8 8 0 0 1-3 7.5"/><path d="M16 22l.6-1.6L15 19.7"/>',
};

function icon(name, cls)
{
  const svg = document.createElementNS(SVG_NS, "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("fill", "none");
  svg.setAttribute("stroke", "currentColor");
  svg.setAttribute("stroke-width", "1.8");
  svg.setAttribute("stroke-linecap", "round");
  svg.setAttribute("stroke-linejoin", "round");
  svg.setAttribute("aria-hidden", "true");
  if (cls)
    svg.setAttribute("class", cls);
  svg.innerHTML = ICONS[name];
  return svg;
}

// ---------- cards ----------

const SUIT_SYMBOL = { H: "♥", D: "♦", S: "♠", C: "♣" };
const SUIT_NAME = { H: "hearts", D: "diamonds", S: "spades", C: "clubs" };
const RANK_LABEL = { T: "10" };
const RANK_NAME = { A: "Ace", K: "King", Q: "Queen", J: "Jack", T: "Ten", 9: "Nine", 8: "Eight", 7: "Seven", 6: "Six" };

const suit = s => h("span", { class: s === "H" || s === "D" ? "red" : null }, SUIT_SYMBOL[s]);
const cardLabel = c => [RANK_LABEL[c[0]] ?? c[0], suit(c[1])];
const cardName = c => `${RANK_NAME[c[0]]} of ${SUIT_NAME[c[1]]}`;

// table.js backSvg(), which src/paint/CardRenderer.cpp back() paints.
function backSvg()
{
  const w = 100, hgt = 145, cx = 50, cy = 72.5;
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${w} ${hgt}">
    <defs><pattern id="p" width="9" height="9" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
      <rect width="9" height="9" fill="#7a1c1c"/><path d="M0 4.5h9M4.5 0v9" stroke="#d2ac63" stroke-width=".5" opacity=".55"/><circle cx="4.5" cy="4.5" r="1.4" fill="#a8392b"/></pattern></defs>
    <rect width="${w}" height="${hgt}" rx="7" fill="#f2e8d2"/>
    <rect x="4.5" y="4.5" width="${w - 9}" height="${hgt - 9}" rx="4.5" fill="#7a1c1c"/>
    <rect x="8" y="8" width="${w - 16}" height="${hgt - 16}" rx="3" fill="url(#p)" stroke="#d8b56a" stroke-width="1"/>
    <ellipse cx="${cx}" cy="${cy}" rx="15" ry="21" fill="#7a1c1c" stroke="#d8b56a" stroke-width="1.2"/>
    <path d="M${cx} ${cy - 10} L${cx + 6} ${cy} L${cx} ${cy + 10} L${cx - 6} ${cy} Z" fill="#d8b56a"/></svg>`;
  return `url("data:image/svg+xml,${encodeURIComponent(svg)}")`;
}

// ---------- the desktop client's geometry ----------

// src/table/TableSpace.h and TableView::fitTable(): the fit box, scaled uniformly
// into the view below the menu bar, inside its margins, and centred.
const VIEW_W = 1920, VIEW_H = 1034;
const FIT = { x: -172, y: -150, w: 1880, h: 1324 };
const S = Math.min((VIEW_W - 32) / FIT.w, (VIEW_H - 18) / FIT.h);
const OX = 16 + ((VIEW_W - 32) - FIT.w * S) / 2 - FIT.x * S;
const OY = 10 + ((VIEW_H - 18) - FIT.h * S) / 2 - FIT.y * S;
const HUD = 1.2 * S;   // tablespace::hudScale, at the table's scale

const CX = 768, CY = 512;
const deg = r => r * 180 / Math.PI;
const onCircle = (a, r) => [CX + r * Math.cos(a), CY + r * Math.sin(a)];

// src/table/SeatLayout.cpp: the panel corners per player count, clockwise from
// the seat after yours. Yours is always at the bottom.
const CORNERS = {
  2: [[628, -140]],
  3: [[96, 49], [1160, 49]],
  4: [[-34, 453], [628, -140], [1290, 453]],
  5: [[-24, 611], [96, 49], [1160, 49], [1280, 611]],
  6: [[1, 700], [96, 49], [628, -140], [1160, 49], [1255, 700]],
};

function panelRect(n, steps)
{
  if (steps === 0)
    return { x: 768 - 259 / 2, y: 1046, w: 259, h: 118 };
  const [x, y] = CORNERS[n][steps - 1];
  return { x: x + 140 - 259 / 2, y, w: 259, h: 118 };   // a 259-wide panel centred in its 280-wide spot
}

// seatlayout::rackAngle(): towards the panel, slid clockwise round the rim when
// the panel would cover the rack.
function rackAngle(p)
{
  const covered = a =>
  {
    const [x, y] = onCircle(a, 452);
    return x > p.x - 10 && x < p.x + p.w + 10 && y > p.y - 10 && y < p.y + p.h + 10;
  };
  let a = Math.atan2(p.y + p.h / 2 - CY, p.x + p.w / 2 - CX);
  if (covered(a))
  {
    for (let k = 0; k < 90 && covered(a); k++)
      a += Math.PI / 180;
    a += 9 * Math.PI / 180;
  }
  return a;
}

function places(n)
{
  return Array.from({ length: n }, (_, s) =>
  {
    const panel = panelRect(n, s);
    return { panel, human: s === 0, angle: s === 0 ? null : rackAngle(panel) };
  });
}

// src/table/TrickItem.cpp: each played card touches the logo's rim, turned to its owner.
function playedPlace(n, place)
{
  const hgt = n <= 4 ? 205.2 : 181.2, w = hgt * 500 / 726, r = 107.5 + hgt / 2;
  let cx = CX, cy = CY + r, rot = 0;
  if (!place.human)
  {
    [cx, cy] = onCircle(place.angle, r);
    rot = deg(place.angle) - 90;
  }
  return { x: cx - w / 2, y: cy - hgt / 2, w, h: hgt, rot };
}

function leadPlace(n, place)
{
  if (place.human)
  {
    const hgt = n <= 4 ? 205.2 : 181.2;
    return { x: CX, y: (CY + 107.5 + hgt + 850) / 2, rot: 0 };
  }
  const [x, y] = onCircle(place.angle, 416);
  return { x, y, rot: deg(place.angle) - 90 };
}

// src/table/HandItem.cpp handitem::cardPlacement(), turned about the hinge below the card.
const HAND_W = 154.9, HAND_H = 225;
function handPlace(count, slot)
{
  const gaps = Math.max(count - 1, 0);
  const step = count > 1 ? Math.max(0, Math.min(84, (720 - HAND_W) / gaps)) : 0;
  const span = gaps * step + HAND_W;
  const fromMiddle = slot - gaps / 2;
  return { x: CX - span / 2 + slot * step, y: 850 + 1.6 * fromMiddle * fromMiddle, rot: 1.9 * fromMiddle };
}

// ---------- the people at the table ----------

const AVATAR = id => AVATARS[id].src;

const PEOPLE = [
  { name: "Andrei", avatar: "man-stubble" },               // you, the host
  { name: "Ioana", avatar: "woman-long-dark-hair" },
  { name: "Cyborg", avatar: "cyborg-man-eye", level: "Cyborg" },
  { name: "Ana", avatar: "woman-long-red-hair" },
  { name: "Bogdan", avatar: "man-short-hair" },
];
const N = PEOPLE.length;
const BID_ORDER = [2, 3, 4, 0, 1];   // round 11: the Cyborg bids first
const BIDS = [1, 0, 2, 1, 2];
const TOTALS = [34, 31, 45, 22, 27];
const RUNS = [["w", 2], ["l", 1], ["w", 3], ["l", 2], ["w", 1]];
const TABLE_CODE = "K7QM4P";

// ---------- the parts of a table page ----------

function menubar(kind)
{
  const item = (name, label, cls) => h("button", { class: "mb" + (cls ? " " + cls : ""), tabindex: "-1" }, icon(name), label);
  const bar = h("nav", { class: "menubar", "aria-label": "Game menu" });
  if (kind === "online")
  {
    bar.append(item("settings", "Settings"), item("rules", "Rules"),
               h("span", { class: "mb code push" }, "Table ", h("b", null, TABLE_CODE)),
               item("globe", "English"), item("exit", "Leave table"));
  }
  else
  {
    bar.append(item("newGame", "New Game"), item("settings", "Settings"), item("rules", "Rules"),
               item("globe", "English", "push"));
  }
  return bar;
}

function ornament(lower)
{
  const svg = document.createElementNS(SVG_NS, "svg");
  svg.setAttribute("width", "104");
  svg.setAttribute("height", "11");
  svg.setAttribute("viewBox", "0 0 120 12");
  svg.setAttribute("fill", "none");
  svg.setAttribute("stroke", "currentColor");
  svg.setAttribute("stroke-width", "1.2");
  svg.setAttribute("aria-hidden", "true");
  svg.innerHTML = lower
    ? '<path d="M2 6 C20 0 30 12 48 6 M72 6 C90 0 100 12 118 6"/><path d="M60 1 L65 6 L60 11 L55 6 Z" fill="currentColor"/>'
    : '<path d="M2 6 H48 M72 6 H118"/><path d="M60 1 L65 6 L60 11 L55 6 Z" fill="currentColor"/><circle cx="50" cy="6" r="1.6" fill="currentColor"/><circle cx="70" cy="6" r="1.6" fill="currentColor"/>';
  return svg;
}

function medal(cls)
{
  return h("div", { class: "medal " + cls, role: "img", "aria-label": "Romanian Whist" },
           h("div", null, ornament(false), h("div", { class: "letters" }, "Romanian", h("br"), "Whist"), ornament(true)));
}

function rack(angle, count)
{
  const [x, y] = onCircle(angle, 452);
  const el = h("div", { class: "rack", style: `left:${x}px;top:${y}px;transform:rotate(${deg(angle) - 90}deg)` });
  const mid = (count - 1) / 2;
  for (let i = 0; i < count; i++)
  {
    el.append(h("div", { class: "back",
                         style: `transform:translateX(${(i - mid) * 26}px) rotate(${(i - mid) * -2}deg) rotateX(50deg)` }));
  }
  const width = Math.max(0, count - 1) * 26 + 78;
  el.append(h("div", { class: "ledge", style: `left:${-width / 2}px;width:${width}px` }));
  return el;
}

function playedCard(n, place, played)
{
  const p = playedPlace(n, place);
  return h("div", { class: "tcard" + (played.winning ? " winning" : ""),
                    style: `left:${p.x}px;top:${p.y}px;width:${p.w}px;height:${p.h}px;transform:rotate(${p.rot}deg)` },
           h("div", { class: "inner" }, h("img", { src: CARD_IMG[played.card], alt: cardName(played.card) })));
}

function leadMark(n, place)
{
  const p = leadPlace(n, place);
  return h("div", { class: "leadpip", title: "Led this trick",
                    style: `left:${p.x}px;top:${p.y}px;transform:rotate(${p.rot}deg) scaleY(1.45) rotate(45deg)` });
}

function pill(...content)
{
  return h("span", { class: "pill" }, ...content);
}
const takenBid = (taken, bid) => pill(`Taken/Bid: ${taken}/${bid}`);

// The turn's countdown: a ring round the panel and the seconds above it (plan
// sections 5.3 and 8.4). `left` of `out of` seconds.
function countdown(left, outOf)
{
  const w = 259 + 14, hgt = 118 + 14;
  const svg = document.createElementNS(SVG_NS, "svg");
  svg.setAttribute("class", "ring" + (left <= 5 ? " late" : ""));
  svg.setAttribute("width", w);
  svg.setAttribute("height", hgt);
  svg.setAttribute("aria-hidden", "true");
  const shape = `x="2" y="2" width="${w - 4}" height="${hgt - 4}" rx="14" pathLength="100"`;
  svg.innerHTML = `<rect class="track" ${shape}/><rect class="left" ${shape} stroke-dasharray="100 100" stroke-dashoffset="${100 * (1 - left / outOf)}"/>`;
  const secs = h("span", { class: "timer" + (left <= 5 ? " late" : "") }, `0:${String(left).padStart(2, "0")}`);
  return { ring: svg, secs,
           set(value)
           {
             svg.querySelector(".left").setAttribute("stroke-dashoffset", 100 * (1 - Math.max(0, value) / outOf));
             svg.classList.toggle("late", value <= 5);
             secs.classList.toggle("late", value <= 5);
             secs.textContent = `0:${String(Math.max(0, value)).padStart(2, "0")}`;
           } };
}

function seatPanel(seat, rect)
{
  const cls = ["panel", "seat"];
  if (seat.active) cls.push("active");
  if (seat.away) cls.push("away");
  if (seat.open) cls.push("open");
  const el = h("div", { class: cls.join(" "), style: `left:${rect.x}px;top:${rect.y}px` });
  if (seat.open)
  {
    el.append(h("div", { class: "portrait" }, icon("plus")),
              h("div", { class: "who" }, h("div", { class: "name" }, "Open seat"), seat.action));
    return el;
  }
  const person = seat.person;
  el.append(h("div", { class: "portrait" }, h("img", { src: AVATAR(person.avatar), alt: "" })),
            h("div", { class: "who" }, h("div", { class: "name" }, person.name), seat.pill));
  if (seat.replaced)
    el.append(h("span", { class: "badge", title: "A computer player has this seat" }, icon("chip")));
  if (seat.countdown)
    el.append(seat.countdown.ring, seat.countdown.secs);
  return el;
}

function trickPanel(info)
{
  return h("div", { class: "panel hud trickinfo", style: `left:8px;top:8px;transform:scale(${HUD});transform-origin:0 0` },
           h("div", { class: "title" }, info.title),
           (info.rows ?? []).map(row => h("div", { class: "row" }, row)),
           info.prompt ? h("div", { class: "prompt" + (info.tone ? " " + info.tone : "") }, info.prompt) : null);
}

function scoreboard(sc)
{
  const head = h("tr", null, ["Player", "B", "T", "Pts", "Run"].map(t => h("th", null, t)));
  const rows = sc.rows.map(r => h("tr", null,
    h("td", { class: r.turn ? "turn" : null }, r.name, r.computer ? icon("chip", "chip") : null),
    h("td", null, r.b ?? "–"), h("td", null, r.t ?? "–"), h("td", null, r.pts),
    h("td", { class: r.run[0] }, `${r.run[0].toUpperCase()}${r.run[1]}`)));
  return h("div", { class: "panel hud score", style: `right:8px;top:8px;transform:scale(${HUD});transform-origin:100% 0` },
           h("div", { class: "round" }, `Round ${sc.round} of ${sc.of}`),
           h("div", { class: "sub" }, `${sc.cards} ${sc.cards === 1 ? "card" : "cards"} dealt · ${sc.type}`),
           h("div", { class: "tension" }, sc.tension),
           h("table", null, h("thead", null, head), h("tbody", null, rows)),
           h("div", { class: "divider" }),
           h("div", { class: "trump" }, h("div", { class: "trumplabel" }, "Trump"),
             sc.trump ? h("img", { src: CARD_IMG[sc.trump], alt: cardName(sc.trump) }) : h("div", { class: "none" }, "None this round")));
}

function tableButtons()
{
  return h("div", { class: "tbtns", style: `transform:scale(${S})` },
           h("button", { class: "panel tbtn", tabindex: "-1" }, icon("history"), "Round history"),
           h("button", { class: "panel tbtn", tabindex: "-1" }, icon("last"), "Last trick"));
}

function notice(iconName, ...text)
{
  return h("div", { class: "panel notice", role: "status" }, icon(iconName), h("span", null, ...text));
}

// One table page: the menu bar, then the view with the table and its corner panels.
// `t` holds the seats, the racks, the trick, the hand and whatever sits on top.
function tablePage(t)
{
  const pl = places(N);
  const stage = h("div", { class: "stage", style: `transform:translate(${OX}px,${OY}px) scale(${S})` },
                  h("div", { class: "wood" }), h("div", { class: "felt" }, h("div", { class: "felt-line" })),
                  medal("on-table"));
  t.seats.forEach((seat, s) =>
  {
    if (s > 0 && seat.cards)
      stage.append(rack(pl[s].angle, seat.cards));
  });
  for (const played of t.trick ?? [])
    stage.append(playedCard(N, pl[played.seat], played));
  if (t.lead !== undefined && t.lead !== null)
    stage.append(leadMark(N, pl[t.lead]));
  (t.hand ?? []).forEach((card, i) => stage.append(handCard(t, card, i)));
  t.seats.forEach((seat, s) => stage.append(seatPanel(seat, pl[s].panel)));
  if (t.onStage)
    stage.append(t.onStage);

  const view = h("div", { class: "view" }, stage);
  if (t.left) view.append(t.left);
  if (t.right) view.append(t.right);
  if (t.buttons !== false) view.append(tableButtons());
  if (t.notice) view.append(t.notice);
  if (t.sheet) view.append(h("div", { class: "scrim" }, t.sheet));
  return [menubar("online"), view];
}

// Your hand, in the desktop client's order (src/core/CardOrder.cpp): the trump's
// suit first, then spades, hearts, clubs, diamonds, high to low.
function handCard(t, entry, i)
{
  const count = t.hand.length;
  const p = handPlace(count, i);
  const state = entry.legal === undefined ? "" : entry.legal ? " legal" : " illegal";
  const el = h("button", { class: "hcard" + state, tabindex: entry.legal ? "0" : "-1",
                           "aria-label": cardName(entry.card) + (entry.legal === false ? ", not playable" : entry.legal ? ", playable" : ""),
                           style: `--x:${p.x}px;--y:${p.y}px;--rot:${p.rot}deg;z-index:${10 + i}` },
               h("div", { class: "face" }, h("img", { src: CARD_IMG[entry.card], alt: "" })));
  if (entry.legal && t.onPlay)
    el.addEventListener("click", () => t.onPlay(entry.card, el));
  return el;
}

// Your card flies to its place by the logo, then the page renders the next state.
function flyToTable(el, done)
{
  const target = playedPlace(N, { human: true });
  const k = target.w / HAND_W;
  const x = target.x - (1 - k) * HAND_W / 2;
  const y = target.y - (1 - k) * 1.3 * HAND_H;
  el.classList.add("flying");
  el.style.zIndex = 25;
  el.style.transform = `translate(${x}px, ${y}px) rotate(0deg) scale(${k})`;
  later(matchMedia("(prefers-reduced-motion: reduce)").matches ? 0 : 480, done);
}

// ---------- the round in progress: round 11 of 29, seven cards, trump the 7 of hearts ----------

function scoreRows(taken, turn, computerSeats = [])
{
  return BID_ORDER.map(s => ({ name: PEOPLE[s].name, b: BIDS[s], t: taken[s], pts: TOTALS[s], run: RUNS[s],
                               turn: s === turn, computer: computerSeats.includes(s) }));
}

function roundScore(taken, turn, computerSeats)
{
  return { round: 11, of: 29, cards: 7, type: "Normal", trump: "7H",
           tension: ["Bids total ", h("b", null, "6"), " for 7 tricks: under by 1"],
           rows: scoreRows(taken, turn, computerSeats) };
}

function seatsFor({ taken, active, cards, replaced = [], away = null, timers = {} })
{
  return PEOPLE.map((person, s) =>
  {
    const seat = { person, active: s === active, cards: cards[s], replaced: replaced.includes(s) };
    if (s === away)
    {
      seat.away = true;
      seat.pill = h("span", { class: "pill away" }, icon("away"), "Away");
    }
    else
    {
      seat.pill = takenBid(taken[s], BIDS[s]);
    }
    if (timers[s])
      seat.countdown = timers[s];
    return seat;
  });
}

// ---------- the screens ----------

let live = [];   // timers belonging to the screen on show
function later(ms, fn) { live.push(setTimeout(fn, ms)); }
function every(ms, fn) { live.push(setInterval(fn, ms)); }
function stopLive() { live.forEach(id => { clearTimeout(id); clearInterval(id); }); live = []; }

const app = document.getElementById("app");
function show(...parts)
{
  app.replaceChildren(...parts.flat().filter(Boolean));
}

const HAND = ["AH", "TH", "JC", "QD", "8D"];

// Trick 3: Bogdan's turn, and he has gone away. The table waits out his grace period.
function mountAway()
{
  const taken = [0, 0, 1, 0, 1];
  const trick = [{ seat: 2, card: "9D" }, { seat: 3, card: "KD", winning: true }];
  const timer = countdown(23, 30);
  let left = 23;

  const draw = gone =>
  {
    const seats = gone
      ? seatsFor({ taken, active: 0, cards: [0, 5, 4, 4, 4], replaced: [4] })
      : seatsFor({ taken, active: 4, cards: [0, 5, 4, 4, 5], away: 4, timers: { 4: timer } });
    show(tablePage({
      seats,
      trick: gone ? [...trick, { seat: 4, card: "6D" }] : trick,
      lead: 2,
      hand: HAND.map(card => ({ card })),
      left: trickPanel({ title: "Trick 3 of 7",
                         rows: [["Leader: ", h("b", null, "Cyborg"), " · Led ", suit("D")],
                                ["Winning: ", h("b", null, "Ana, ", cardLabel("KD"))]],
                         prompt: gone ? ["Your turn. You must follow ", suit("D"), "."] : "Bogdan is reconnecting…",
                         tone: gone ? null : "wait" }),
      right: scoreboard(roundScore(taken, gone ? 0 : 4, gone ? [4] : [])),
      notice: gone
        ? notice("chip", "Bogdan didn't come back in time. ", h("b", null, "A computer player"), " is playing for him.")
        : notice("away", h("b", null, "Bogdan"), " lost his connection. The table is waiting for him."),
    }));
  };
  draw(false);
  every(1000, () =>
  {
    left -= 1;
    timer.set(left);
    if (left === 0)
      draw(true);
    if (left <= 0)
      stopLive();
  });
}

// Trick 3: your turn, with the turn timer's last 15 seconds on your panel.
function mountTurn()
{
  const taken = [0, 0, 1, 0, 1];
  const trick = [{ seat: 2, card: "9D" }, { seat: 3, card: "KD", winning: true }, { seat: 4, card: "6D" }];
  const timer = countdown(14, 15);
  let left = 14;

  const prompt = ["Your turn. You must follow ", suit("D"), "."];
  const waiting = () => show(tablePage({
    seats: seatsFor({ taken, active: 0, cards: [0, 5, 4, 4, 4], timers: { 0: timer } }),
    trick, lead: 2,
    hand: HAND.map(card => ({ card, legal: card[1] === "D" })),
    onPlay: (card, el) => { stopLive(); flyToTable(el, () => played(card, false)); },
    left: trickPanel({ title: "Trick 3 of 7",
                       rows: [["Leader: ", h("b", null, "Cyborg"), " · Led ", suit("D")],
                              ["Winning: ", h("b", null, "Ana, ", cardLabel("KD"))]],
                       prompt }),
    right: scoreboard(roundScore(taken, 0)),
    notice: notice("back", h("b", null, "Bogdan"), " is back."),
  }));

  const played = (card, forYou) => show(tablePage({
    seats: seatsFor({ taken, active: 1, cards: [0, 5, 4, 4, 4] }),
    trick: [...trick, { seat: 0, card }], lead: 2,
    hand: HAND.filter(c => c !== card).map(c => ({ card: c })),
    left: trickPanel({ title: "Trick 3 of 7",
                       rows: [["Leader: ", h("b", null, "Cyborg"), " · Led ", suit("D")],
                              ["Winning: ", h("b", null, "Ana, ", cardLabel("KD"))]],
                       prompt: "Ioana is thinking…" }),
    right: scoreboard(roundScore(taken, 1)),
    notice: forYou ? notice("clock", "Time ran out. Your computer player played the ", h("b", null, cardLabel(card)), " for you.") : null,
  }));

  waiting();
  every(1000, () =>
  {
    left -= 1;
    timer.set(left);
    if (left === 0)
    {
      stopLive();
      played("8D", true);
    }
  });
}

// Trick 4: Ana has left. Her seat's computer player leads for her.
function mountStandIn()
{
  const taken = [0, 0, 1, 1, 1];
  const base = {
    hand: ["AH", "TH", "JC", "8D"].map(card => ({ card })),
    notice: notice("chip", h("b", null, "Ana"), " left the table. A computer player is playing for her."),
  };
  show(tablePage({ ...base,
    seats: seatsFor({ taken, active: 3, cards: [0, 4, 4, 4, 4], replaced: [3] }),
    trick: [], lead: 3,
    left: trickPanel({ title: "Trick 4 of 7", rows: [["Leader: ", h("b", null, "Ana")], ["Winning: ", h("b", null, "–")]],
                       prompt: "Computer playing for Ana…", tone: "stand" }),
    right: scoreboard(roundScore(taken, 3, [3])),
  }));
  later(2600, () => show(tablePage({ ...base,
    seats: seatsFor({ taken, active: 4, cards: [0, 4, 4, 3, 4], replaced: [3] }),
    trick: [{ seat: 3, card: "TS", winning: true }], lead: 3,
    left: trickPanel({ title: "Trick 4 of 7",
                       rows: [["Leader: ", h("b", null, "Ana"), " · Led ", suit("S")], ["Winning: ", h("b", null, "Ana, ", cardLabel("TS"))]],
                       prompt: "Bogdan is thinking…" }),
    right: scoreboard(roundScore(taken, 4, [3])),
  })));
}

// The end of round 11: the round sheet, held until every connected player continues.
function mountHold()
{
  const taken = [1, 1, 2, 2, 1];
  const after = [40, 30, 52, 21, 26];
  const runs = [["w", 3], ["l", 2], ["w", 4], ["l", 3], ["l", 1]];
  const ready = { you: false, ioana: true, bogdan: false };
  let left = 24;

  const chip = (label, on) => h("span", { class: "who-chip" + (on ? " ready" : "") }, on ? icon("check") : null, label);
  const secs = h("span", { class: "secs" }, "0:24");
  const bar = h("div", null);
  bar.style.width = `${left / 30 * 100}%`;
  const status = h("span", null);
  const chips = h("span", { class: "line" });
  const go = h("button", { class: "sbtn primary", onclick: () => { ready.you = true; refresh(); later(3500, () => { ready.bogdan = true; refresh(); }); } }, "Continue");

  const refresh = () =>
  {
    const all = ready.you && ready.ioana && ready.bogdan;
    status.textContent = all || left <= 0 ? "Dealing round 12…" : "The next round starts when everyone has pressed Continue, or in";
    secs.hidden = all || left <= 0;
    chips.replaceChildren(h("span", { class: "hint", style: "color:var(--muted)" }, "Ready:"),
                          chip("You", ready.you), chip("Ioana", ready.ioana), chip("Bogdan", ready.bogdan));
    go.disabled = ready.you;
    go.textContent = ready.you ? (all ? "Continue" : "Waiting for Bogdan…") : "Continue";
    if (all || left <= 0)
      stopLive();
  };

  const rows = PEOPLE.map((p, s) =>
  {
    const diff = after[s] - TOTALS[s];
    return h("tr", { class: s === 0 ? "you" : null },
             h("td", { class: "l" }, p.name), h("td", null, BIDS[s]), h("td", null, taken[s]),
             h("td", { class: diff >= 0 ? "plus" : "minus" }, diff >= 0 ? `+${diff}` : `−${-diff}`),
             h("td", null, `${TOTALS[s]} → ${after[s]}`));
  });
  const sheet = h("div", { class: "panel sheet", role: "dialog", "aria-label": "Round 11" },
    h("h2", null, "Round 11 · 7 cards · Normal"),
    h("div", { class: "meta" }, "Trump 7", suit("H"), " · exact bid scores 5 + bid, a miss loses the difference"),
    h("table", null,
      h("thead", null, h("tr", null, h("th", { class: "l" }, "Player"), h("th", null, "Bid"), h("th", null, "Taken"), h("th", null, "Round"), h("th", null, "Total"))),
      h("tbody", null, rows)),
    h("div", { class: "hold" },
      h("div", { class: "line" }, icon("hourglass"), status, secs, h("span", { class: "grow" })),
      h("div", { class: "bar" }, bar),
      chips,
      h("div", { class: "note" }, "Ana has left, so her computer player doesn't hold up the table. Nor would anyone whose connection had dropped.")),
    h("div", { class: "actions" }, h("button", { class: "sbtn" }, "Round history"), go));

  const seats = PEOPLE.map((person, s) => ({ person, pill: takenBid(taken[s], BIDS[s]), replaced: s === 3, cards: 0 }));
  show(tablePage({
    seats,
    trick: [{ seat: 1, card: "7D" }, { seat: 2, card: "KS", winning: true }, { seat: 3, card: "TS" }, { seat: 4, card: "6D" }, { seat: 0, card: "8D" }],
    left: trickPanel({ title: "Round over", prompt: "Scores are in." }),
    right: scoreboard({ round: 11, of: 29, cards: 7, type: "Normal", trump: "7H",
                        tension: ["Bids total ", h("b", null, "6"), " for 7 tricks: under by 1"],
                        rows: BID_ORDER.map(s => ({ name: PEOPLE[s].name, b: BIDS[s], t: taken[s], pts: after[s], run: runs[s], computer: s === 3 })) }),
    sheet,
  }));
  refresh();
  every(1000, () =>
  {
    left -= 1;
    secs.textContent = `0:${String(Math.max(0, left)).padStart(2, "0")}`;
    bar.style.width = `${Math.max(0, left) / 30 * 100}%`;
    if (left <= 0)
      refresh();
  });
}

function mountResults()
{
  const finals = [[1, 112], [2, 104], [0, 97], [4, 81], [3, 66]];
  const sheet = h("div", { class: "panel sheet", role: "dialog", "aria-label": "Final results" },
    h("h2", null, "Final results"),
    h("div", { class: "meta" }, "Ioana wins with 112 points."),
    h("table", null,
      h("thead", null, h("tr", null, h("th", { class: "l" }, "Place"), h("th", { class: "l" }, "Player"), h("th", null, "Total"))),
      h("tbody", null, finals.map(([s, total], i) => h("tr", { class: i === 0 ? "you" : null },
        h("td", { class: "l" }, `${i + 1}.`), h("td", { class: "l" }, PEOPLE[s].name, s === 0 ? h("span", { style: "color:var(--muted);font-weight:400" }, " (you)") : null),
        h("td", null, total, i === 0 ? h("span", { class: "star" }, "★") : h("span", { class: "star", style: "visibility:hidden" }, "★")))))),
    h("div", { class: "actions" },
      h("button", { class: "sbtn" }, "Leave table"), h("button", { class: "sbtn" }, "Round history"),
      h("button", { class: "sbtn primary" }, "New game, same seats")));
  const seats = PEOPLE.map((person, s) => ({ person, pill: takenBid(s === 2 ? 1 : 0, s === 1 ? 1 : 0), replaced: s === 3, cards: 0 }));
  show(tablePage({
    seats,
    trick: [{ seat: 2, card: "AH", winning: true }, { seat: 3, card: "TH" }, { seat: 4, card: "JC" }, { seat: 0, card: "QD" }, { seat: 1, card: "KS" }],
    left: trickPanel({ title: "Game over", prompt: "Final scores are in." }),
    right: scoreboard({ round: 29, of: 29, cards: 1, type: "Hidden", trump: "7D",
                        tension: ["Bids total ", h("b", null, "1"), " for 1 trick"],
                        rows: [4, 0, 1, 2, 3].map(s => ({ name: PEOPLE[s].name, b: s === 1 ? 1 : 0, t: s === 2 ? 1 : 0,
                                                          pts: finals.find(f => f[0] === s)[1],
                                                          run: [["l", 1], ["w", 1], ["l", 1], ["l", 2], ["w", 2]][s], computer: s === 3 })) }),
    sheet,
  }));
}

// The table before the game, from the host's seat.
function mountLobby()
{
  let filled = false;
  const draw = () =>
  {
    const seats = PEOPLE.map((person, s) =>
    {
      if (s === 0) return { person, pill: h("span", { class: "pill host" }, "Host · you") };
      if (s === 2) return { person, pill: h("span", { class: "pill computer" }, icon("chip"), "Cyborg ▾") };
      if (s === 4)
      {
        if (!filled)
          return { open: true, action: h("button", { class: "small-btn", onclick: () => { filled = true; draw(); } }, icon("chip"), "Add computer") };
        return { person: { name: "Elena", avatar: "woman-shoulder-hair" },
                 pill: h("button", { class: "pill computer", style: "background:none;cursor:pointer", title: "Open the seat again",
                                     onclick: () => { filled = false; draw(); } }, icon("chip"), "Reckoner (Beta) ▾") };
      }
      return { person, pill: pill(h("span", { class: "dot" }), "Here") };
    });

    const copy = h("button", { class: "sbtn", onclick: () =>
    {
      const text = `https://whist.example/t/${TABLE_CODE}`;
      try { navigator.clipboard.writeText(text).catch(() => {}); } catch (error) { /* copying is a nicety here */ }
      copy.lastChild.textContent = "Copied";
    } }, icon("copy"), h("span", null, "Copy link"));

    const invite = h("div", { class: "panel invite" },
      h("div", { class: "label" }, "Invite players"),
      h("div", { class: "code", "aria-label": "Table code K 7 Q M 4 P" }, TABLE_CODE),
      h("div", { class: "link" }, h("div", { class: "field" }, `whist.example/t/${TABLE_CODE}`), copy),
      h("p", null, "Anyone with the link, or the code, can take an open seat."),
      h("div", { class: "rule" }),
      h("button", { class: "sbtn primary start", onclick: () => go("away") }, "Start the game"),
      h("p", null, filled ? "Every seat is taken." : "4 of 5 seats taken. The open seat will get a computer player."));

    const facts = (title, pairs, foot) => h("div", { class: "facts" },
      h("div", { class: "title" }, title),
      h("dl", null, pairs.map(([k, v]) => [h("dt", null, k), h("dd", null, v)])),
      foot ? h("div", { class: "edit" }, foot) : null);

    const left = facts(`Table ${TABLE_CODE}`, [["Host", "You"], ["Seats", filled ? "5 of 5 taken" : "4 of 5 taken"]],
                       "Waiting for players. Start when you're ready.");
    left.className = "panel hud facts";
    left.style.cssText = `left:8px;top:8px;transform:scale(${HUD});transform-origin:0 0`;

    const right = facts("Table settings", [
      ["Game", "1-8-1, 29 rounds"],
      ["Last", "A forehead and a hidden round"],
      ["Bots", "Normal speed"],
      ["Turns", "60 s each"],
      ["Rounds", "Wait for everyone, 30 s at most"],
      ["Drops", "Reckoner (Beta) takes over"],
    ], "Change settings");
    right.className = "panel hud facts";
    right.style.cssText = `right:8px;top:8px;transform:scale(${HUD});transform-origin:100% 0`;

    show(tablePage({ seats, onStage: invite, left, right, buttons: false }));
  };
  draw();
}

// ---------- the pages before a table ----------

function menuPage(dim)
{
  const button = (name, label, note) => h("button", { class: "panel tbtn", tabindex: "-1" }, icon(name), label, h("small", null, note));
  const page = h("div", { class: "view" },
    h("div", { class: "menupage" },
      h("div", { class: "logo" }, medal("")),
      h("div", { class: "buttons" },
        button("newGame", "New Game", "against the computer"),
        button("people", "Create a table", "online"),
        button("link", "Join a table", "online")),
      h("div", { class: "foot" },
        h("span", null, icon("globe"), "English · Română"),
        h("span", null, icon("code"), "Source code"),
        h("span", null, icon("info"), "About and privacy"))));
  return [menubar("menu"), page, dim];
}

const label = text => h("div", { class: "section-label" }, text);
const select = (text, width) => h("span", { class: "input select", style: width ? `min-width:${width}px` : null }, text);
const field = (text, cls = "name") => h("span", { class: "input " + cls }, text);
const opt = (on, text, radio) => h("div", { class: "opt" }, h("span", { class: (radio ? "dot-radio" : "box") + (on ? " on" : "") }), text);
const row = (...kids) => h("div", { class: "frow" }, ...kids);

function speedSlider(stop)
{
  const track = h("span", { class: "track" });
  for (let i = 0; i < 5; i++)
    track.append(h("i", { style: `left:${i * 25}%` }));
  track.append(h("span", { class: "knob", style: `left:${stop * 25}%` }));
  return h("div", { class: "slider" }, h("span", { class: "lab", style: "min-width:118px;color:var(--muted)" }, "Bot speed"),
           h("span", { class: "end" }, "Slow"), track, h("span", { class: "end" }, "Instant"));
}

function portraitGrid(chosen)
{
  const ids = Object.keys(AVATARS).filter(id => !AVATARS[id].strategy && AVATARS[id].gender === "male");
  return h("div", { class: "portraits" },
           ids.map(id => h("span", { class: "pic" + (id === chosen ? " on" : "") }, h("img", { src: AVATAR(id), alt: "" }))));
}

function sheetOver(sheet)
{
  return h("div", { class: "scrim", style: "top:46px" }, sheet);
}

function mountMenu()
{
  show(menuPage(null));
}

// The desktop client's New Game (src/ui/NewGameDialog.cpp), as a page over the menu.
function mountSolo()
{
  const bot = (n, name, gender, level) => row(h("span", { style: "width:26px;color:var(--muted)" }, `${n}.`),
                                             field(name), select(gender, 112), h("span", { style: "color:var(--muted)" }, "plays"), select(level, 210));
  const sheet = h("div", { class: "panel sheet", style: "width:800px", role: "dialog", "aria-label": "New Game" },
    h("h2", null, "New Game"),
    h("div", { class: "form", style: "gap:11px" },
      row(opt(true, "I'm playing", true), h("span", { style: "width:24px" }), opt(false, "I'm watching", true)),
      row(h("span", { class: "lab" }, "Your name"), field("Andrei"), select("Man", 112)),
      row(h("span", { class: "lab" }, "Opponents"), field("3", "num"), h("span", { class: "hint" }, "(2-6 at the table)")),
      bot(1, "Emma", "Woman", "Reckoner (Beta)"),
      bot(2, "Cyborg", "Man", "Cyborg"),
      bot(3, "Drake", "Man", "Ducking"),
      label("Shape of the game"),
      opt(true, "1-8-1 - counts up from one card and back down", true),
      opt(false, "8-1-8 - the other way round", true),
      opt(true, "End with a forehead round and a hidden round"),
      opt(false, "Play every one-card round forehead"),
      h("div", { class: "summary" }, "26 rounds - about 13 minutes"),
      label("Pace"),
      speedSlider(2),
      opt(true, "Wait for me after each round"),
      row(opt(false, "Fixed deal, seed"), field("42", "num"))),
    h("div", { class: "actions" }, h("button", { class: "sbtn", onclick: () => go("menu") }, "Cancel"),
      h("button", { class: "sbtn primary" }, "Deal")));
  show(menuPage(sheetOver(sheet)));
}

function mountCreate()
{
  const sheet = h("div", { class: "panel sheet", style: "width:1120px", role: "dialog", "aria-label": "Create a table" },
    h("h2", null, "Create a table"),
    h("div", { class: "meta" }, "You'll get a link to send. People who open it take the open seats."),
    h("div", { class: "form" },
      h("div", { class: "cols" },
        h("div", { class: "col" },
          label("You"),
          row(h("span", { class: "lab" }, "Name"), field("Andrei"), select("Man", 112)),
          h("div", { class: "lab", style: "color:var(--muted);margin-bottom:-4px" }, "Portrait"), portraitGrid("man-stubble"),
          label("The table"),
          row(h("span", { class: "lab" }, "Seats"), h("span", { class: "seg" }, [2, 3, 4, 5, 6].map(n => h("span", { class: n === 5 ? "on" : null }, n)))),
          h("div", { class: "frow", style: "color:var(--muted);font-size:16px" }, "You'll choose who sits where, and which seats go to computer players, in the lobby."),
          label("Shape of the game"),
          opt(true, "1-8-1 - counts up from one card and back down", true),
          opt(false, "8-1-8 - the other way round", true),
          opt(true, "End with a forehead round and a hidden round"),
          opt(false, "Play every one-card round forehead"),
          h("div", { class: "summary" }, "29 rounds - about 15 minutes")),
        h("div", { class: "col" },
          label("Pace"),
          speedSlider(2),
          opt(true, "Wait for everyone after each round, 30 s at most"),
          label("When someone drops"),
          row(h("span", { class: "lab" }, "Replaced by"), select("Reckoner (Beta)", 220)),
          h("div", { class: "frow", style: "color:var(--muted);font-size:16px" }, "A player who loses their connection gets 30 seconds to come back. After that a computer player keeps their seat, and hands it back when they return."),
          row(h("span", { class: "lab" }, "Turn timer"), select("60 seconds", 160)),
          h("div", { class: "frow", style: "color:var(--muted);font-size:16px" }, "A move not made in time is made by the player's computer player. Three in a row count as leaving."),
          h("div", { class: "frow", style: "color:var(--muted);font-size:16px;margin-top:6px" }, "Online games have no fixed deal: the seed would give the cards away.")))),
    h("div", { class: "actions" }, h("button", { class: "sbtn", onclick: () => go("menu") }, "Cancel"),
      h("button", { class: "sbtn primary", onclick: () => go("lobby") }, "Create table")));
  show(menuPage(sheetOver(sheet)));
}

function mountJoin()
{
  const sheet = h("div", { class: "panel sheet", style: "width:760px", role: "dialog", "aria-label": "Join a table" },
    h("h2", null, "Join a table"),
    h("div", { class: "meta" }, "You opened a link to a table. Choose how the others will see you."),
    h("div", { class: "form" },
      label("Table"),
      row(h("span", { class: "codeboxes", "aria-label": "Code K7QM4P" }, [...TABLE_CODE].map(c => h("span", null, c)))),
      h("div", { class: "found" }, icon("check"), "Andrei's table · 4 of 5 seats taken · 1-8-1, 29 rounds"),
      label("You"),
      row(h("span", { class: "lab" }, "Name"), field("Bogdan"), select("Man", 112)),
      row(h("span", { class: "lab", style: "align-self:flex-start;padding-top:4px" }, "Portrait"), portraitGrid("man-short-hair")),
      h("div", { class: "error-line" }, "Your name and portrait stay in this browser. There are no accounts or passwords.")),
    h("div", { class: "actions" }, h("button", { class: "sbtn", onclick: () => go("menu") }, "Cancel"),
      h("button", { class: "sbtn primary", onclick: () => go("lobby") }, "Take a seat")));
  show(menuPage(sheetOver(sheet)));
}

function mountPhone(host)
{
  host.replaceChildren(h("div", { class: "phone", role: "img", "aria-label": "A phone held upright, asking to be turned" },
    h("div", { class: "inner" }, icon("turn", "turn"),
      h("h3", null, "Turn your phone sideways"),
      h("p", null, "The table needs a wide screen. Held sideways, a phone plays the whole game."))));
}

// ---------- the viewer: tabs, captions, scaling ----------

const SCREENS = [
  { id: "menu", group: "Before the game", tab: "Main menu", url: "whist.example", mount: mountMenu,
    title: "The main menu",
    text: "The desktop client's main menu, with two ways to play online added. New Game plays against computer players, as on the desktop. The other two set up a table, or join one, that friends reach by a link.",
    refs: "Plan sections 2.2, 7, 9 (the source link the AGPL needs)" },
  { id: "solo", tab: "New Game", url: "whist.example", mount: mountSolo,
    title: "New Game, against the computer",
    text: "The desktop client's New Game, unchanged: play or watch, the opponents and their level, the game's shape, the pace and a fixed deal. The game runs on the server, as a private table with one human seat.",
    refs: "Plan sections 4.1, 7.1" },
  { id: "create", tab: "Create a table", url: "whist.example/new", mount: mountCreate,
    title: "Creating an online table",
    text: "The same settings, without watching and without the seed. Two are new: which computer player takes a seat when someone drops, and the turn timer. You also pick a name, a gender (Romanian sentences need it) and a portrait.",
    refs: "Plan sections 7.2, 7.3; decisions D9, D10, D12, D16" },
  { id: "join", tab: "Join by link", url: `whist.example/t/${TABLE_CODE}`, mount: mountJoin,
    title: "Joining by link",
    text: "Someone who opens the link lands here with the code filled in. Names are unique at a table, and there are no accounts: a random token in the browser is what lets you back into your seat.",
    refs: "Plan sections 7.3, 9 (identity)" },
  { id: "lobby", tab: "Lobby", url: `whist.example/t/${TABLE_CODE}`, mount: mountLobby,
    title: "The lobby",
    text: "The table before the game, from the host's seat. Each seat is open, taken by a person, or given to a computer player. The host can change the settings, the seats and the computer players' levels until the game starts.",
    tryText: "Try Add computer on the open seat (right), and Copy link.",
    refs: "Plan sections 4.9, 7.2" },
  { id: "away", group: "At the table, online", tab: "A player drops", url: `whist.example/t/${TABLE_CODE}`, mount: mountAway,
    title: "A player drops during his turn",
    text: "Bogdan's connection dropped while it was his turn. The table waits up to 30 seconds for him, and everyone sees the countdown on his panel. If he doesn't come back, the computer player behind his seat moves for him, and keeps doing so until he returns.",
    tryText: "Wait for the countdown to run out.",
    refs: "Plan sections 5.1, 5.2 (timelines 1 to 3), 5.3" },
  { id: "turn", tab: "Your turn", url: `whist.example/t/${TABLE_CODE}`, mount: mountTurn,
    title: "Your turn, with the turn timer",
    text: "Bogdan made it back. Online, each turn has 60 seconds; the gold ring and the seconds above your panel show the last 15. Otherwise the table is the desktop client's: the cards you can't play are greyed out and the ones you can are raised.",
    tryText: "Click a raised card to play it, or let the timer run out.",
    refs: "Plan sections 5.2 (timeline 4), 8.4, decision D9" },
  { id: "stand-in", tab: "Computer stands in", url: `whist.example/t/${TABLE_CODE}`, mount: mountStandIn,
    title: "A computer player stands in",
    text: "Ana left the table. Her seat keeps her name, portrait and score; the badge on her portrait says a computer player has it, and the trick panel says so when it moves. If she comes back by the same link, her next move is hers again.",
    refs: "Plan sections 3.2, 5.2 (timelines 5, 6), 5.3" },
  { id: "hold", tab: "Round hold", url: `whist.example/t/${TABLE_CODE}`, mount: mountHold,
    title: "Waiting for everyone after a round",
    text: "Online, “Wait for me after each round” becomes a shared hold: the round sheet stays up until every connected player has pressed Continue, or for 30 seconds. Nobody can pause an online game.",
    tryText: "Press Continue.",
    refs: "Plan section 4.8, decisions D12, D13" },
  { id: "results", tab: "Final results", url: `whist.example/t/${TABLE_CODE}`, mount: mountResults,
    title: "Final results",
    text: "The desktop client's final results. Online, the host can start another game at the same table with the same seats, so nobody needs a new link.",
    refs: "Plan section 4.9" },
  { id: "phone", tab: "Phone held upright", url: "", mount: null,
    title: "A phone held upright",
    text: "The first version supports phones held sideways, where the table scales down as it does in a small desktop window. Held upright, the page asks you to turn the phone.",
    refs: "Plan section 8.10, decision D14" },
];

const tabs = document.getElementById("tabs");
const frame = document.getElementById("frame");
const phoneShot = document.getElementById("phone-shot");
const screenBox = document.getElementById("screen");
const tabButtons = {};

app.style.setProperty("--wood-tex", `url("${WOOD_TEX}")`);
app.style.setProperty("--felt-tex", `url("${FELT_TEX}")`);
app.style.setProperty("--back", backSvg());

{
  let group = null;
  for (const screen of SCREENS)
  {
    if (screen.group)
    {
      group = h("div", { class: "tabrow" });
      tabs.append(h("div", { class: "tabgroup" }, h("span", { class: "group" }, screen.group), group));
    }
    const button = h("button", { class: "tab", type: "button", onclick: () => go(screen.id) }, screen.tab);
    tabButtons[screen.id] = button;
    group.append(button);
  }
}

function go(id)
{
  if (location.hash !== `#${id}`)
    location.hash = id;
  else
    render(id);
}

function render(id)
{
  const screen = SCREENS.find(s => s.id === id) ?? SCREENS[0];
  stopLive();
  for (const [key, button] of Object.entries(tabButtons))
    button.setAttribute("aria-current", key === screen.id ? "true" : "false");
  document.getElementById("cap-title").textContent = screen.title;
  document.getElementById("cap-text").textContent = screen.text;
  document.getElementById("cap-refs").textContent = screen.refs;
  const tryLine = document.getElementById("cap-try");
  tryLine.hidden = !screen.tryText;
  tryLine.textContent = screen.tryText ?? "";
  document.getElementById("url").textContent = screen.url;

  const phone = screen.id === "phone";
  frame.hidden = phone;
  phoneShot.hidden = !phone;
  if (phone)
    mountPhone(phoneShot);
  else
    screen.mount();
}

function fit()
{
  app.style.transform = `scale(${screenBox.clientWidth / 1920})`;
}
new ResizeObserver(fit).observe(screenBox);
fit();

window.addEventListener("hashchange", () => render(location.hash.slice(1)));
render(location.hash.slice(1) || "menu");
