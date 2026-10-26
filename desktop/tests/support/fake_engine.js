// A stand-in for the engine's setlist commands (engine/host/src/setlist_controller.cpp semantics, ws.v1 JSON in and out) for the Story 9.13 tests: the same limits, the same
// typed errors, full-permutation reorders, add-before-remove cross moves. `engine.request(msg)` is what `window.veloraDevice.request` is in the page.
import { SENT_MESSAGES } from "../../src/setlists_view.js";

export const CODE = { NO_SUCH_LIVE_SET: 20, NO_SUCH_SETLIST: 22, NO_SUCH_TAG: 23, DUPLICATE: 24, INVALID_NAME: 25, LIMIT: 29, INVALID_ORDER: 31, NOT_IN_SETLIST: 32, STORAGE: 33, STORAGE_FULL: 36, BUSY: 13, UPSTREAM: 44, INVALID_COMMAND: 5 };
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;

const utf8 = (s) => new TextEncoder().encode(s).length;
const nameOk = (s) => typeof s === "string" && s.length > 0 && utf8(s) <= 64 && !/[\u0000-\u001f\u007f]/.test(s);

// All Live Sets order: names compared with ASCII letters folded, then the id (byte order), as live_set_name_less does.
const fold = (n) => n.replace(/[a-z]/g, (c) => c.toUpperCase());
const byNameThenId = (x, y) => (fold(x.name) < fold(y.name) ? -1 : fold(x.name) > fold(y.name) ? 1 : x.id < y.id ? -1 : x.id > y.id ? 1 : 0);

export class FakeEngine {
  constructor({ liveSets = [], setlists = [], tags = [] } = {}) {
    this.liveSets = liveSets.map((x) => ({ ...x })); // { id, name } (a Live Set listed here exists)
    this.setlists = setlists.map((s) => ({ id: s.id, name: s.name, ids: s.ids.slice(), tagIds: (s.tagIds || []).slice() }));
    this.tags = tags.map((t) => ({ ...t }));
    this.loaded = "";
    this.revision = 1;
    this.log = []; // [name, body] of every request, in order
    this.fail = null; // (name, body) => error code | null : makes the next matching command fail with that typed error
    this.nextId = 1;
    this.delay = 0;
  }

  mint(prefix) {
    if (prefix === "ls") return `00000000-0000-4000-8000-${String(this.nextId++).padStart(12, "0")}`; // Live Set ids are UUIDv4
    return `${prefix}-${this.nextId++}`;
  }

  err(code, message = "", details = {}) {
    return { body: { Error: { code, message, details } } };
  }

  exists(id) {
    return this.liveSets.some((l) => l.id === id);
  }

  async request(msg) {
    if (this.delay) await new Promise((r) => setTimeout(r, this.delay));
    const names = Object.keys(msg.body);
    const name = names[0];
    // Every test doubles as the runtime assertion "never Load / Step / Jump / Recall / Write / DeleteLiveSet": only the messages the view is allowed to send get through.
    if (!SENT_MESSAGES.includes(name)) throw new Error("the Setlists view sent a message it must never send: " + name);
    const b = msg.body[name];
    this.log.push([name, JSON.parse(JSON.stringify(b))]);
    if (this.fail && name !== "GetSetlists") {
      const code = this.fail(name, b);
      if (code) return this.err(code, "injected");
    }
    return this.handle(name, b);
  }

  handle(name, b) {
    const sl = (id) => this.setlists.find((s) => s.id === id);
    const ack = () => { this.revision++; return { body: { Ack: { not_durable: false } } }; };
    switch (name) {
      case "GetSetlists": return this.read(b);
      case "CreateSetlist":
        if (!nameOk(b.name)) return this.err(CODE.INVALID_NAME);
        if (this.setlists.length >= 100) return this.err(CODE.LIMIT, "", { limit_name: "setlists", limit: "100" });
        { const id = this.mint("sl"); this.setlists.push({ id, name: b.name, ids: [], tagIds: [] }); this.revision++; return { body: { CreatedReply: { id, name: b.name, not_durable: false } } }; }
      case "RenameSetlist":
        if (!sl(b.id)) return this.err(CODE.NO_SUCH_SETLIST, "", { id: b.id });
        if (!nameOk(b.name)) return this.err(CODE.INVALID_NAME);
        sl(b.id).name = b.name; return ack();
      case "ReorderSetlists": {
        const cur = this.setlists.map((s) => s.id);
        if (!Array.isArray(b.ids) || b.ids.length !== cur.length || new Set(b.ids).size !== cur.length || !b.ids.every((i) => cur.includes(i))) return this.err(CODE.INVALID_ORDER);
        this.setlists = b.ids.map((i) => sl(i)); return ack();
      }
      case "DeleteSetlist":
        if (!sl(b.id)) return this.err(CODE.NO_SUCH_SETLIST, "", { id: b.id });
        this.setlists = this.setlists.filter((s) => s.id !== b.id); if (this.loaded === b.id) this.loaded = ""; return ack();
      case "AddLiveSetToSetlist": {
        const s = sl(b.setlist_id);
        if (!s) return this.err(CODE.NO_SUCH_SETLIST, "", { id: b.setlist_id });
        if (!UUID.test(b.live_set_id) || !this.exists(b.live_set_id)) return this.err(CODE.NO_SUCH_LIVE_SET, "", { id: b.live_set_id });
        if (s.ids.includes(b.live_set_id)) return this.err(CODE.DUPLICATE, "", { setlist_id: b.setlist_id, live_set_id: b.live_set_id });
        if (s.ids.length >= 256) return this.err(CODE.LIMIT, "", { limit_name: "live_sets_per_setlist", limit: "256" });
        if (b.index !== undefined && b.index > s.ids.length) return this.err(CODE.INVALID_ORDER);
        s.ids.splice(b.index === undefined ? s.ids.length : b.index, 0, b.live_set_id); return ack();
      }
      case "RemoveLiveSetFromSetlist": {
        const s = sl(b.setlist_id);
        if (!s) return this.err(CODE.NO_SUCH_SETLIST, "", { id: b.setlist_id });
        if (!s.ids.includes(b.live_set_id)) return this.err(CODE.NOT_IN_SETLIST);
        s.ids = s.ids.filter((i) => i !== b.live_set_id); return ack();
      }
      case "ReorderLiveSetsInSetlist": {
        const s = sl(b.setlist_id);
        if (!s) return this.err(CODE.NO_SUCH_SETLIST, "", { id: b.setlist_id });
        const ids = b.live_set_ids;
        if (!Array.isArray(ids) || ids.length !== s.ids.length || new Set(ids).size !== s.ids.length || !ids.every((i) => s.ids.includes(i))) return this.err(CODE.INVALID_ORDER);
        s.ids = ids.slice(); return ack();
      }
      case "CreateLiveSet": {
        if (!nameOk(b.name)) return this.err(CODE.INVALID_NAME);
        const id = this.mint("ls"); this.liveSets.push({ id, name: b.name }); this.revision++;
        return { body: { CreatedReply: { id, name: b.name, not_durable: false } } };
      }
      case "CreateTag":
        if (!nameOk(b.name)) return this.err(CODE.INVALID_NAME);
        if (this.tags.length >= 64) return this.err(CODE.LIMIT, "", { limit_name: "tags", limit: "64" });
        { const id = this.mint("tag"); this.tags.push({ id, name: b.name }); this.revision++; return { body: { CreatedReply: { id, name: b.name, not_durable: false } } }; }
      case "RenameTag": {
        const t = this.tags.find((x) => x.id === b.id);
        if (!t) return this.err(CODE.NO_SUCH_TAG, "", { id: b.id });
        if (!nameOk(b.name)) return this.err(CODE.INVALID_NAME);
        t.name = b.name; return ack();
      }
      case "DeleteTag":
        if (!this.tags.some((x) => x.id === b.id)) return this.err(CODE.NO_SUCH_TAG, "", { id: b.id });
        this.tags = this.tags.filter((x) => x.id !== b.id); this.setlists.forEach((s) => { s.tagIds = s.tagIds.filter((i) => i !== b.id); }); return ack();
      case "SetSetlistTags": {
        const s = sl(b.setlist_id);
        if (!s) return this.err(CODE.NO_SUCH_SETLIST, "", { id: b.setlist_id });
        if (b.tag_ids.length > 8) return this.err(CODE.LIMIT, "", { limit_name: "tags_per_setlist", limit: "8" }); // the limit is checked before the ids
        if (new Set(b.tag_ids).size !== b.tag_ids.length) return this.err(CODE.INVALID_ORDER); // duplicate ids
        if (!b.tag_ids.every((i) => this.tags.some((t) => t.id === i))) return this.err(CODE.NO_SUCH_TAG);
        s.tagIds = b.tag_ids.slice(); return ack();
      }
      default: return this.err(CODE.INVALID_COMMAND, "not a setlist command in this fake: " + name);
    }
  }

  read(b) {
    const detail = b.setlist_id ? this.setlists.find((s) => s.id === b.setlist_id) : null;
    if (b.setlist_id && !detail) return this.err(CODE.NO_SUCH_SETLIST, "", { id: b.setlist_id });
    const entry = (id) => { const l = this.liveSets.find((x) => x.id === id); return { live_set_id: id, name: l ? l.name : "", missing: !l }; };
    const reply = {
      setlists: this.setlists.map((s) => ({ id: s.id, name: s.name, tag_ids: s.tagIds.slice(), live_set_count: s.ids.length, missing_count: s.ids.filter((i) => !this.exists(i)).length, loaded: s.id === this.loaded })),
      tags: this.tags.map((t) => ({ ...t })),
      status: { available: true, loaded_setlist_id: this.loaded, library_revision: this.revision },
      detail_setlist_id: detail ? detail.id : "",
      entries: detail ? detail.ids.map(entry) : [],
    };
    if (b.include_all_live_sets) reply.all_live_sets = this.liveSets.slice().sort(byNameThenId).map((l) => ({ live_set_id: l.id, name: l.name, missing: false }));
    return { body: { SetlistsReply: reply } };
  }
}

export function sample() {
  return new FakeEngine({
    liveSets: [["Alpha", "a"], ["Bravo", "b"], ["Charlie", "c"], ["Delta", "d"], ["Echo", "e"]].map(([n, ch]) => ({ id: `${ch.repeat(8)}-${ch.repeat(4)}-4${ch.repeat(3)}-8${ch.repeat(3)}-${ch.repeat(12)}`, name: n })), // UUIDv4-shaped ids, as the store mints
    tags: [{ id: "t-gig", name: "Gig" }, { id: "t-jazz", name: "Jazz" }],
    setlists: [
      { id: "s-one", name: "Friday", ids: ["aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "cccccccc-cccc-4ccc-8ccc-cccccccccccc"], tagIds: ["t-gig"] },
      { id: "s-two", name: "Saturday", ids: ["dddddddd-dddd-4ddd-8ddd-dddddddddddd"] },
      { id: "s-three", name: "Sunday", ids: ["ffffffff-ffff-4fff-8fff-ffffffffffff", "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"] }, // ffffffff-ffff-4fff-8fff-ffffffffffff: a Live Set that no longer exists (kept, flagged)
    ],
  });
}
