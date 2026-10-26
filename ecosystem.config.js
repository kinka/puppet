const os = require("os");
const path = require("path");

const root = __dirname;
const uv = path.join(os.homedir(), ".local", "bin", "uv");

module.exports = {
  apps: [
    {
      name: "stackchan-host",
      cwd: root,
      script: uv,
      args: "run --script host/stackchan_host.py",
      interpreter: "none",
      env: {
        STACKCHAN_BLE: "0",
      },
      autorestart: true,
      restart_delay: 2000,
    },
    {
      name: "stackchan-quota",
      cwd: root,
      script: uv,
      args: "run --script scripts/cpa_quota_screen.py",
      interpreter: "none",
      autorestart: true,
      restart_delay: 2000,
    },
  ],
};
