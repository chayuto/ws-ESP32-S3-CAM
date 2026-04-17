(() => {
  const $ = id => document.getElementById(id);
  $('host').textContent = location.host;

  const history = [];
  const HISTORY_N = 120;
  const canvas = $('chart');
  const ctx = canvas.getContext('2d');

  /* ---------- chart ---------- */
  function drawChart() {
    const W = canvas.width, H = canvas.height;
    ctx.clearRect(0, 0, W, H);
    ctx.strokeStyle = '#2a2f3a';
    ctx.beginPath();
    for (let y = 0; y <= 1; y += 0.25) {
      const py = H - y * H;
      ctx.moveTo(0, py); ctx.lineTo(W, py);
    }
    ctx.stroke();
    if (history.length < 2) return;
    ctx.strokeStyle = '#64d18a';
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    for (let i = 0; i < history.length; ++i) {
      const x = (i / (HISTORY_N - 1)) * W;
      const y = H - history[i] * H;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
  }

  /* ---------- metrics rendering ---------- */
  function setState(st, sub) {
    const el = $('state');
    el.textContent = st.toUpperCase();
    el.className = 'state ' + (
      st === 'crying' ? 'alert' :
      st === 'idle'   ? 'listen' :
      st === 'error'  ? 'alert' :
      st === 'syncing' || st === 'connecting' ? 'warn' : 'idle'
    );
    $('state_sub').textContent = sub || '';
  }
  const pillHTML = (b, t, f) =>
    `<span class="pill ${b ? 'on' : 'warn'}">${b ? t : f}</span>`;
  const humanBytes = n => {
    if (n === undefined) return '—';
    if (n > 1024 * 1024) return (n / 1024 / 1024).toFixed(2) + ' MB';
    if (n > 1024) return (n / 1024).toFixed(1) + ' KB';
    return n + ' B';
  };
  const formatUptime = s => {
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), ss = s % 60;
    return `${h}h ${m}m ${ss}s`;
  };

  function renderMetrics(m) {
    setState(m.state, m.state === 'idle' ? 'listening' :
      m.state === 'crying' ? `alert (last ${m.last_cry_conf.toFixed(2)})` :
      m.state);
    $('m_ms').textContent = m.last_inference_ms;
    $('m_p95').textContent = m.p95_inference_ms;
    $('m_fps').textContent = m.inference_fps.toFixed(2);
    $('m_alerts').textContent = m.alert_count;
    $('m_rms').textContent = m.input_rms.toFixed(0);
    $('m_floor').textContent = m.noise_floor_p95 !== undefined ? m.noise_floor_p95.toFixed(0) : '—';
    $('m_uptime').textContent = formatUptime(m.uptime_s);
    $('m_wifi').innerHTML = pillHTML(m.wifi_connected, 'up', 'down');
    $('m_rssi').textContent = m.wifi_rssi + ' dBm';
    $('m_ntp').innerHTML = pillHTML(m.ntp_synced, 'synced', 'not synced');
    $('m_sd').innerHTML = pillHTML(m.sd_mounted, 'mounted', 'fallback');
    $('m_heap').textContent = `${humanBytes(m.free_heap)} / ${humanBytes(m.free_psram)}`;
    $('m_listeners').textContent = m.stream_listeners !== undefined ? m.stream_listeners : '—';
    $('conf_val').textContent = m.last_cry_conf.toFixed(3);
    $('conf_fill').style.width = Math.min(100, m.last_cry_conf * 100) + '%';
    $('conf_bar').classList.toggle('hi', m.last_cry_conf > 0.5);
    $('listen_count').textContent =
      (m.stream_listeners > 0) ? `${m.stream_listeners} listener(s) active` : '';
  }

  function appendLog(kind, obj) {
    const li = document.createElement('li');
    if (kind === 'detect') li.className = 'alert';
    const ts = new Date().toLocaleTimeString();
    const conf = obj.conf !== undefined ? `conf=${obj.conf.toFixed(2)}` : '';
    let html = `<span>${ts}</span><span>${kind}</span><span>${conf}</span>`;
    if (obj.wav) {
      html += `<a href="/recordings/${obj.wav}" target="_blank">▶ play</a>`;
    }
    li.innerHTML = html;
    const list = $('log');
    list.insertBefore(li, list.firstChild);
    while (list.children.length > 30) list.removeChild(list.lastChild);
  }

  function pushConf(c) {
    history.push(c);
    while (history.length > HISTORY_N) history.shift();
    drawChart();
  }

  /* ---------- SSE ---------- */
  function connectSSE() {
    const es = new EventSource('/events');
    es.addEventListener('snapshot', e => {
      try { renderMetrics(JSON.parse(e.data)); } catch (_) {}
      $('conn').textContent = 'live';
    });
    es.addEventListener('inference', e => {
      try { pushConf(JSON.parse(e.data).conf); } catch (_) {}
    });
    es.addEventListener('detect', e => {
      try { appendLog('detect', JSON.parse(e.data)); } catch (_) {}
    });
    es.addEventListener('heartbeat', () => {});
    es.onerror = () => { $('conn').textContent = 'disconnected'; };
  }

  async function pollMetrics() {
    try {
      const r = await fetch('/metrics');
      if (r.ok) renderMetrics(await r.json());
    } catch (_) {}
  }

  /* ---------- live audio via Web Audio API ---------- */
  let audioCtx = null, workletNode = null, reader = null, abortCtrl = null;

  async function startListening() {
    try {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 16000 });
      await audioCtx.audioWorklet.addModule(URL.createObjectURL(new Blob([`
        class PCMPlayer extends AudioWorkletProcessor {
          constructor() { super(); this.buf = []; this.port.onmessage = e => this.buf.push(e.data); }
          process(_in, out) {
            const ch = out[0][0];
            for (let i = 0; i < ch.length; ++i) {
              if (this.buf.length === 0) { ch[i] = 0; continue; }
              const head = this.buf[0];
              ch[i] = head.shift() / 32768;
              if (head.length === 0) this.buf.shift();
            }
            return true;
          }
        }
        registerProcessor('pcm-player', PCMPlayer);
      `], { type: 'text/javascript' })));
      workletNode = new AudioWorkletNode(audioCtx, 'pcm-player');
      workletNode.connect(audioCtx.destination);

      abortCtrl = new AbortController();
      const res = await fetch('/audio.pcm', { signal: abortCtrl.signal });
      if (!res.ok) throw new Error('stream fetch failed ' + res.status);
      reader = res.body.getReader();

      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        const i16 = new Int16Array(value.buffer, value.byteOffset, value.byteLength >> 1);
        workletNode.port.postMessage(Array.from(i16));
      }
    } catch (e) {
      console.error(e);
    } finally {
      stopListening();
    }
  }

  function stopListening() {
    if (abortCtrl) { abortCtrl.abort(); abortCtrl = null; }
    if (reader) { try { reader.cancel(); } catch (_) {} reader = null; }
    if (workletNode) { workletNode.disconnect(); workletNode = null; }
    if (audioCtx) { audioCtx.close(); audioCtx = null; }
    const btn = $('btn_listen');
    btn.textContent = '🔊 Listen';
    btn.classList.remove('on');
    $('listen_status').textContent = 'Tap to hear the room in real time.';
    $('privacy_note').textContent = '';
  }

  $('btn_listen').addEventListener('click', () => {
    const btn = $('btn_listen');
    if (btn.classList.contains('on')) {
      stopListening();
    } else {
      btn.textContent = '⏸ Stop';
      btn.classList.add('on');
      $('listen_status').textContent = 'Streaming live audio from the bedroom…';
      $('privacy_note').textContent = 'LED on the device is breathing — streaming active.';
      startListening();
    }
  });

  /* ---------- LED brightness ---------- */
  async function fetchBrightness() {
    try {
      const r = await fetch('/led/brightness');
      if (!r.ok) return;
      const j = await r.json();
      $('led_slider').value = j.brightness;
      $('led_val').textContent = j.brightness;
    } catch (_) {}
  }
  async function setBrightness(pct) {
    try {
      const r = await fetch('/led/brightness?pct=' + pct);
      if (r.ok) {
        const j = await r.json();
        $('led_slider').value = j.brightness;
        $('led_val').textContent = j.brightness;
      }
    } catch (_) {}
  }
  $('led_slider').addEventListener('change', e => setBrightness(e.target.value));
  $('led_night').addEventListener('click', () => setBrightness(5));
  $('led_off').addEventListener('click', () => setBrightness(0));
  $('led_full').addEventListener('click', () => setBrightness(100));

  /* ---------- bootstrap ---------- */
  drawChart();
  pollMetrics();
  fetchBrightness();
  setInterval(pollMetrics, 2000);
  connectSSE();
})();
