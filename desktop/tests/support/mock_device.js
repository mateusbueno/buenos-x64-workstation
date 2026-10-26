// The browser harness (Story 9.9): stands in for the Tauri side so `desktop/src` can be opened in a plain browser with a paired "unit" (tests/support/fake_unit.js behind
// window.veloraDevice). Served by tests/support/serve_mock.py, which adds this module in front of app.js. `window.__mockUnit` is the unit (inspect `.log`, `.perf`, `.parts`).
import { FakeUnit } from "./fake_unit.js";

const unit = new FakeUnit();
window.__mockUnit = unit;
const listeners = new Map();
const emit = (name, payload) => (listeners.get(name) || []).forEach((cb) => cb({ payload }));
const status = { configured: true, status: "paired", host: "127.0.0.1", port: 7410, model: "DEV HOST", firmware_version: "1.4.0", device_id: "mock" };

window.__TAURI__ = {
  core: {
    invoke: async (cmd, args) => {
      switch (cmd) {
        case "device_status": return status;
        case "device_state": return unit.state();
        case "device_request": return unit.request(args.request);
        default: throw new Error("not mocked: " + cmd);
      }
    },
  },
  event: { listen: async (name, cb) => { listeners.set(name, [...(listeners.get(name) || []), cb]); return () => {}; } },
};
unit.onState((st) => emit("device://state", st));
setTimeout(() => emit("device://state", unit.state()), 50);
