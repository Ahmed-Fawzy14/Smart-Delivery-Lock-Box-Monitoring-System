const express = require("express");
let fcmToken = null;

const fs = require("fs");
const path = require("path");
const admin = require("firebase-admin");

const app = express();

const PORT = process.env.PORT || 8080;
const IMAGES_DIR = path.join(__dirname, "received_images");

const serviceAccount = JSON.parse(process.env.FIREBASE_SERVICE_ACCOUNT);

admin.initializeApp({
  credential: admin.credential.cert(serviceAccount),
});

const FCM_TOKEN = "dySokNrxH0uziWD66hkMCF:APA91bH3ss8TqOp6ZeeUpNOHeP-nA9JtxyllbnOfBu_QBEvV0n-99waZBpUZ-YiE7oMzK2U7fDxE3mY1k58IOgHmmpZ4L-lJzGl31kefKaCuYUaVIy5Y-2E";

let pendingCommand = "none";
let lockStatus = "locked";

if (!fs.existsSync(IMAGES_DIR)) {
  fs.mkdirSync(IMAGES_DIR);
}

async function sendPushNotification(timestamp) {
  if (!fcmToken) {
    console.warn("[NOTIFY] No FCM token registered yet");
    return;
  }
  const message = {
    notification: {
      title: "New Delivery!",
      body: `Someone is at your door`,
    },
    token: fcmToken,
  };
  try {
    await admin.messaging().send(message);
    console.log("[NOTIFY] Push notification sent!");
  } catch (err) {
    console.error("[NOTIFY] Failed:", err.message);
  }
}
app.post("/upload", (req, res) => {
  const timestamp = req.headers["x-timestamp"] || `no-timestamp-${Date.now()}`;
  const filename = `image_${timestamp}.jpg`;
  const filepath = path.join(IMAGES_DIR, filename);
  pendingCommand = "none";

  const chunks = [];

  req.on("data", (chunk) => chunks.push(chunk));

  req.on("end", () => {
    const imageBuffer = Buffer.concat(chunks);

    if (imageBuffer.length === 0) {
      console.warn(`[UPLOAD] Empty body received at ${timestamp}`);
      return res.status(400).send("Empty image body");
    }

    fs.writeFile(filepath, imageBuffer, (err) => {
      if (err) {
        console.error(`[UPLOAD] Failed to save image: ${err.message}`);
        return res.status(500).send("Failed to save image");
      }

      console.log(`[UPLOAD] Saved: ${filename} (${imageBuffer.length} bytes)`);
      sendPushNotification(timestamp);
      res.status(200).send("Image received");
    });
  });

  req.on("error", (err) => {
    console.error(`[UPLOAD] Request error: ${err.message}`);
    res.status(500).send("Request error");
  });
});

app.get("/images", (req, res) => {
  fs.readdir(IMAGES_DIR, (err, files) => {
    if (err) {
      return res.status(500).json({ error: "Could not read images directory" });
    }

    const jpegFiles = files
      .filter((f) => f.endsWith(".jpg"))
      .sort()
      .reverse();

    const host = req.headers.host;
    const protocol = req.headers["x-forwarded-proto"] || "http";

    const images = jpegFiles.map((filename) => {
      const timestamp = filename.replace("image_", "").replace(".jpg", "");
      return {
        filename,
        timestamp,
        url: `${protocol}://${host}/images/${filename}`,
      };
    });

    res.json(images);
  });
});

app.get("/images/:filename", (req, res) => {
  const filename = req.params.filename;

  if (filename.includes("/") || filename.includes("..")) {
    return res.status(400).send("Invalid filename");
  }

  const filepath = path.join(IMAGES_DIR, filename);

  if (!fs.existsSync(filepath)) {
    return res.status(404).send("Image not found");
  }

  res.setHeader("Content-Type", "image/jpeg");
  res.sendFile(filepath);
});

app.get("/latest", (req, res) => {
  fs.readdir(IMAGES_DIR, (err, files) => {
    if (err) return res.status(500).send("Could not read images directory");

  const jpegFiles = files
    .filter((f) => f.endsWith(".jpg"))
    .sort((a, b) => {
      const statA = fs.statSync(path.join(IMAGES_DIR, a)).mtimeMs;
      const statB = fs.statSync(path.join(IMAGES_DIR, b)).mtimeMs;
      return statB - statA;
    });
  
  if (jpegFiles.length === 0) {
    return res.status(404).send("No images yet");
  }
  
  const latest = jpegFiles[0];
    const filepath = path.join(IMAGES_DIR, latest);

    res.setHeader("Content-Type", "image/jpeg");
    res.setHeader("X-Filename", latest);
    res.sendFile(filepath);
  });
});

app.post("/unlock", (req, res) => {
  pendingCommand = "1";
  lockStatus = "unlocked";
  console.log("[COMMAND] Unlock requested by mobile app");
  res.status(200).json({ status: "unlock command queued" });
});

app.post("/lock", (req, res) => {
  pendingCommand = "0";
  lockStatus = "locked";
  console.log("[COMMAND] Lock requested by mobile app");
  res.status(200).json({ status: "lock command queued" });
});

app.get("/command", (req, res) => {
  const cmd = pendingCommand;
  pendingCommand = "none";
  console.log(`[COMMAND] ESP32-CAM polled. Sending: ${cmd}`);

  if (cmd === "1") {
    res.status(200).send("true");
  } else if (cmd === "0") {
    res.status(200).send("false");
  } else {
    res.status(200).send("none");
  }
});

app.post("/status", express.json(), (req, res) => {
  const { status } = req.body;
  if (status === "locked" || status === "unlocked") {
    lockStatus = status;
    console.log(`[STATUS] Lock is now: ${lockStatus}`);
    res.status(200).json({ received: true });
  } else {
    res.status(400).json({ error: "Invalid status value" });
  }
});

app.get("/status", (req, res) => {
  res.status(200).json({ status: lockStatus });
});

app.get("/gallery", (req, res) => {
  fs.readdir(IMAGES_DIR, (err, files) => {
    if (err) return res.status(500).send("Could not read images directory");

    const host = req.headers.host;
    const protocol = req.headers["x-forwarded-proto"] || "http";

  const jpegFiles = files
    .filter((f) => f.endsWith(".jpg"))
    .sort((a, b) => {
      const statA = fs.statSync(path.join(IMAGES_DIR, a)).mtimeMs;
      const statB = fs.statSync(path.join(IMAGES_DIR, b)).mtimeMs;
      return statB - statA;
    });

    const imageCards = jpegFiles.map((filename) => {
      const timestamp = filename.replace("image_", "").replace(".jpg", "");
      const url = `${protocol}://${host}/images/${filename}`;

      let displayTime = timestamp;
      try {
        const [datePart, timePart] = timestamp.split("T");
        const [y, m, d] = datePart.split("-");
        const [hh, mm, ss] = timePart.split("-");
        const months = ["","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"];
        displayTime = `${months[parseInt(m)]} ${d}, ${y} — ${hh}:${mm}:${ss}`;
      } catch (_) {}
      return `
        <div class="card" onclick="openModal('${url}', '${displayTime}')">
          <img src="${url}" alt="${displayTime}" loading="lazy" />
          <div class="timestamp">${displayTime}</div>
        </div>`;
    }).join("");

    const empty = jpegFiles.length === 0
      ? `<div class="empty">📭 No images yet. Waiting for deliveries...</div>`
      : "";

    res.send(`<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0"/>
  <title>Smart Delivery Box — Gallery</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: -apple-system, sans-serif; background: #f0f2f5; color: #222; }
    header {
      background: #3f51b5; color: white; padding: 16px 24px;
      display: flex; align-items: center; justify-content: space-between;
    }
    header h1 { font-size: 18px; font-weight: 600; }
    .badge {
      background: #4caf50; color: white; font-size: 12px;
      padding: 4px 10px; border-radius: 20px;
    }
    .count { color: rgba(255,255,255,0.7); font-size: 13px; margin-top: 2px; }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
      gap: 16px; padding: 24px;
    }
    .card {
      background: white; border-radius: 12px; overflow: hidden;
      box-shadow: 0 2px 8px rgba(0,0,0,0.1); cursor: pointer;
      transition: transform 0.2s, box-shadow 0.2s;
    }
    .card:hover { transform: translateY(-4px); box-shadow: 0 6px 16px rgba(0,0,0,0.15); }
    .card img { width: 100%; height: 200px; object-fit: cover; display: block; }
    .timestamp {
      padding: 10px 14px; font-size: 13px; color: #3f51b5; font-weight: 500;
    }
    .empty {
      text-align: center; padding: 80px 20px; color: #999; font-size: 18px;
    }
    .modal {
      display: none; position: fixed; inset: 0;
      background: rgba(0,0,0,0.85); z-index: 100;
      align-items: center; justify-content: center; flex-direction: column;
    }
    .modal.open { display: flex; }
    .modal img { max-width: 90vw; max-height: 80vh; border-radius: 8px; }
    .modal .caption { color: white; margin-top: 12px; font-size: 14px; }
    .modal .close {
      position: absolute; top: 16px; right: 20px;
      color: white; font-size: 28px; cursor: pointer; font-weight: bold;
    }
  </style>
</head>
<body>
  <header>
    <div>
      <h1>📦 Smart Delivery Box</h1>
      <div class="count">${jpegFiles.length} image${jpegFiles.length !== 1 ? "s" : ""} — auto-refreshes every 5s</div>
    </div>
    <div class="badge">● Live</div>
  </header>

  <div class="grid">${imageCards}${empty}</div>

  <div class="modal" id="modal" onclick="closeModal()">
    <span class="close">✕</span>
    <img id="modalImg" src="" alt="" />
    <div class="caption" id="modalCaption"></div>
  </div>
  <script>
    function openModal(url, caption) {
      document.getElementById('modalImg').src = url;
      document.getElementById('modalCaption').textContent = caption;
      document.getElementById('modal').classList.add('open');
    }
    function closeModal() {
      document.getElementById('modal').classList.remove('open');
    }
    setTimeout(() => location.reload(), 5000);
  </script>
</body>
</html>`);
  });
});

app.post("/register-token", express.json(), (req, res) => {
  const { token } = req.body;
  if (token) {
    fcmToken = token;
    console.log("[TOKEN] FCM token registered");
    res.status(200).json({ received: true });
  } else {
    res.status(400).json({ error: "No token provided" });
  }
});

app.delete("/images/:filename", (req, res) => {
  const filename = req.params.filename;

  if (filename.includes("/") || filename.includes("..")) {
    return res.status(400).send("Invalid filename");
  }

  const filepath = path.join(IMAGES_DIR, filename);

  if (!fs.existsSync(filepath)) {
    return res.status(404).send("Image not found");
  }

  fs.unlink(filepath, (err) => {
    if (err) return res.status(500).send("Failed to delete image");
    console.log(`[DELETE] Removed: ${filename}`);
    res.status(200).send("Image deleted");
  });
});

app.get("/", (req, res) => {
  res.send("Smart Delivery Lock Box Server is running");
});

app.listen(PORT, "0.0.0.0", () => {
  console.log(`\n=== Smart Delivery Lock Box Server ===`);
  console.log(`Listening on port ${PORT}`);
  console.log(`Images saved to: ${IMAGES_DIR}`);
  console.log(`\nEndpoints:`);
  console.log(`  POST /upload`);
  console.log(`  GET  /images`);
  console.log(`  GET  /images/:file`);
  console.log(`  GET  /latest`);
  console.log(`  POST /unlock`);
  console.log(`  POST /lock`);
  console.log(`  GET  /command`);
  console.log(`  POST /status`);
  console.log(`  GET  /status`);
  console.log(`  GET  /gallery`);
  console.log(`  DELETE /images/:file`);
  console.log(`  GET  /`);
  console.log(`\nWaiting for images...\n`);
});
