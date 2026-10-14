/**
 * ==============================================================================
 * SICS Gantry Watering V2.0 - Core Application Logic
 * Peneliti: Azkia Azraei Shahrefi (NIM: 2305041030) - Kelas EK-6B
 * 
 * Terhubung ke Cloud JS Endpoint (/api/sics) pada domain polmedresearchcenter.id
 * ESP32 terus-menerus mengirim telemetri dan mengambil perintah secara otomatis.
 * Jika ESP32 tidak terhubung / belum ada data, seluruh nilai ditampilkan "-".
 * ==============================================================================
 */

// ====== Konstanta Hardware & Dimensi Grid ======
const COLS = 6;
const ROWS = 8;
const X_TOTAL_STEPS = 767.33;
const Y_TOTAL_STEPS = 2118.33;
const X_SPACING = X_TOTAL_STEPS / (COLS - 1); // ~153.47 steps/kolom
const Y_SPACING = Y_TOTAL_STEPS / (ROWS - 1); // ~302.62 steps/baris

// ====== State Global Sistem ======
const state = {
  isConnected: false,
  apiEndpoint: '/api/sics',
  pollingTimer: null,
  pollingInterval: 2000,
  isFetching: false,
  lastSeen: null,

  // Telemetri Real-Time dari ESP32
  temp: null,
  humidity: null,
  soilA: null,
  soilB: null,
  adcA: null,
  adcB: null,
  rtcTime: null,
  posX: null,
  posY: null,
  isMoving: false,
  isScanning: false,
  emergencyStop: false,
  schedules: [],

  // Seleksi Sel Grid UI
  selectedCell: null,

  // Virtual LCD hd44780
  lcdMode: 0,
  lastLcdRotate: Date.now(),
  lcdInterval: 5000
};

// Deteksi Endpoint API (Mendukung file://, localhost, dan domain online)
function resolveApiUrl() {
  const custom = (localStorage.getItem('sics_custom_api') || '').trim();
  const isOnline = window.location.hostname === 'polmedresearchcenter.id' || window.location.hostname === 'www.polmedresearchcenter.id';

  if (custom) {
    if (!isOnline && custom.startsWith('/')) {
      return 'https://polmedresearchcenter.id' + custom;
    }
    return custom;
  }

  // Jika dibuka di server online polmedresearchcenter.id, gunakan path relatif
  if (isOnline) {
    return '/api/sics.php';
  }

  // Jika dibuka dari file lokal atau localhost, arahkan langsung ke endpoint cloud utama
  return 'https://polmedresearchcenter.id/api/sics.php';
}

// ====== Notifikasi Toast ======
function showToast(msg, type = 'info') {
  const area = document.getElementById('toastArea');
  if (!area) return;

  const t = document.createElement('div');
  t.className = `toast toast-${type}`;
  t.textContent = msg;
  area.appendChild(t);

  setTimeout(() => {
    t.style.opacity = '0';
    setTimeout(() => t.remove(), 250);
  }, 3200);
}

// ====== Terminal Serial Log ======
function logEvent(msg, type = 'normal') {
  const now = new Date();
  const timeStr = now.toTimeString().split(' ')[0];
  const terminal = document.getElementById('terminalLogBox');
  if (!terminal) return;

  const row = document.createElement('div');
  row.className = 'log-entry';

  const tSpan = document.createElement('span');
  tSpan.className = 'log-time';
  tSpan.textContent = `[${timeStr}]`;

  const mSpan = document.createElement('span');
  mSpan.className = `log-msg ${type}`;
  mSpan.textContent = msg;

  row.appendChild(tSpan);
  row.appendChild(mSpan);
  terminal.appendChild(row);

  terminal.scrollTop = terminal.scrollHeight;
  while (terminal.children.length > 150) {
    terminal.removeChild(terminal.firstChild);
  }
}

// ====== Inisialisasi Matriks Grid 8x6 (48 Sel) ======
function initGridMatrix() {
  const gridContainer = document.getElementById('gridCells');
  if (!gridContainer) return;
  gridContainer.innerHTML = '';

  for (let r = ROWS - 1; r >= 0; r--) {
    const rowChar = String.fromCharCode(65 + r);
    const isZoneA = r >= 4;

    for (let c = 0; c < COLS; c++) {
      const colNum = c + 1;
      const cellCode = `${rowChar}${colNum}`;

      const cellEl = document.createElement('div');
      cellEl.className = `cell-plant ${isZoneA ? 'is-zone-a' : 'is-zone-b'}`;
      cellEl.id = `cell-${cellCode}`;
      cellEl.textContent = cellCode;
      cellEl.dataset.cell = cellCode;
      cellEl.title = `Sel Tanaman ${cellCode} (${isZoneA ? 'Zona A' : 'Zona B'})`;

      cellEl.addEventListener('click', () => {
        selectCell(cellCode);
      });

      cellEl.addEventListener('dblclick', () => {
        selectCell(cellCode);
        if (state.isConnected) {
          doCommand('GOTO', { cell: cellCode });
        } else {
          showToast('Gagal: ESP32 tidak terhubung (-)', 'error');
          logEvent(`[ERROR] GOTO ${cellCode} ditolak: ESP32 tidak terhubung (-)`, 'error');
        }
      });

      gridContainer.appendChild(cellEl);
    }
  }
}

function selectCell(cellCode) {
  state.selectedCell = cellCode;

  document.querySelectorAll('.cell-plant').forEach(el => {
    el.classList.toggle('selected', el.dataset.cell === cellCode);
  });

  const selectedText = document.getElementById('selectedCellText');
  if (selectedText) {
    selectedText.textContent = cellCode || '-';
  }

  const inputGoto = document.getElementById('inputGoto');
  if (inputGoto && cellCode) {
    inputGoto.value = cellCode;
  }
}

// ====== Inisialisasi Tombol Pintas Baris & Kolom ======
function initRowColButtons() {
  const rowGrid = document.getElementById('rowButtonsGrid');
  const colGrid = document.getElementById('colButtonsGrid');

  if (rowGrid) {
    rowGrid.innerHTML = '';
    for (let r = 0; r < ROWS; r++) {
      const char = String.fromCharCode(65 + r);
      const btn = document.createElement('button');
      btn.className = 'pill-btn';
      btn.textContent = `Row ${char}`;
      btn.title = `Irigasi Baris ${char}`;
      btn.onclick = () => doCommand(`ROW_${char}`);
      rowGrid.appendChild(btn);
    }
  }

  if (colGrid) {
    colGrid.innerHTML = '';
    for (let c = 1; c <= COLS; c++) {
      const btn = document.createElement('button');
      btn.className = 'pill-btn';
      btn.textContent = `Kolom ${c}`;
      btn.title = `Irigasi Kolom ${c}`;
      btn.onclick = () => doCommand(`COL_${c}`);
      colGrid.appendChild(btn);
    }
  }
}

// ====== Update Posisi Toolhead Carriage pada Grid Gantry ======
function updateGantryPosition(xSteps, ySteps, cellName) {
  const carriage = document.getElementById('carriage');
  const railH = document.getElementById('railH');
  const railV = document.getElementById('railV');
  const label = document.getElementById('carriageLabel');
  const spray = document.getElementById('waterSpray');

  if (!carriage) return;

  if (xSteps === null || ySteps === null || !state.isConnected) {
    if (label) label.textContent = '-';
    if (spray) spray.style.display = 'none';
    carriage.style.left = '8.33%';
    carriage.style.top = '6.25%';
    if (railH) railH.style.top = '6.25%';
    if (railV) railV.style.left = '8.33%';
    return;
  }

  const pctX = 8.33 + (xSteps / X_TOTAL_STEPS) * 83.33;
  const pctY = 6.25 + (ySteps / Y_TOTAL_STEPS) * 87.5;

  carriage.style.left = `${Math.min(96, Math.max(4, pctX))}%`;
  carriage.style.top = `${Math.min(96, Math.max(4, pctY))}%`;

  if (railH) railH.style.top = `${pctY}%`;
  if (railV) railV.style.left = `${pctX}%`;

  if (label) {
    label.textContent = cellName || (state.isScanning ? 'SCAN' : '-');
  }

  if (spray) {
    spray.style.display = state.isScanning ? 'block' : 'none';
  }
}

// ====== Render UI Saat Terputus / Tidak Menerima Data dari ESP32 ======
function renderDisconnectedUI() {
  const valTemp = document.getElementById('valTemp');
  const valHum = document.getElementById('valHum');
  const barTemp = document.getElementById('barTemp');
  const barHum = document.getElementById('barHum');
  const valSoilA = document.getElementById('valSoilA');
  const valSoilB = document.getElementById('valSoilB');
  const adcA = document.getElementById('adcValueA');
  const adcB = document.getElementById('adcValueB');

  if (valTemp) valTemp.textContent = '-';
  if (valHum) valHum.textContent = '-';
  if (barTemp) barTemp.style.width = '0%';
  if (barHum) barHum.style.width = '0%';

  if (valSoilA) {
    valSoilA.textContent = '-';
    valSoilA.className = 'badge-soil badge-offline';
  }
  if (valSoilB) {
    valSoilB.textContent = '-';
    valSoilB.className = 'badge-soil badge-offline';
  }
  if (adcA) adcA.textContent = 'Nilai ADC: -';
  if (adcB) adcB.textContent = 'Nilai ADC: -';

  const statusPump = document.getElementById('statusPump');
  const statusMotion = document.getElementById('statusMotion');
  const driverStatus = document.getElementById('driverStatus');
  const coordX = document.getElementById('coordX');
  const coordY = document.getElementById('coordY');
  const estopAlert = document.getElementById('estopAlert');

  if (statusPump) {
    statusPump.textContent = '-';
    statusPump.style.color = 'inherit';
  }
  if (statusMotion) {
    statusMotion.textContent = '-';
    statusMotion.style.color = 'inherit';
  }
  if (driverStatus) {
    driverStatus.textContent = 'Driver: -';
    driverStatus.style.color = '#64748b';
  }
  if (coordX) coordX.textContent = '-';
  if (coordY) coordY.textContent = '-';
  if (estopAlert) estopAlert.style.display = 'none';

  const clockTime = document.getElementById('clockTime');
  const clockDate = document.getElementById('clockDate');
  if (clockTime) clockTime.textContent = '-';
  if (clockDate) clockDate.textContent = '-';

  const modeBadge = document.getElementById('modeBadge');
  const connText = document.getElementById('connText');
  const connDot = document.getElementById('connDot');
  const lastUpdated = document.getElementById('lastUpdated');

  if (modeBadge) {
    modeBadge.textContent = 'Terputus (-)';
    modeBadge.className = 'status-badge offline';
  }
  if (connText) connText.textContent = 'Terputus (-)';
  if (connDot) connDot.className = 'dot-indicator offline';
  if (lastUpdated) lastUpdated.textContent = 'Sinkronisasi: Terputus (-)';

  const line1 = document.getElementById('lcdLine1');
  const line2 = document.getElementById('lcdLine2');
  const modeInd = document.getElementById('lcdModeIndicator');
  if (line1) line1.textContent = '-';
  if (line2) line2.textContent = '-';
  if (modeInd) modeInd.textContent = 'Status: Terputus (-)';

  const schedCount = document.getElementById('schedCount');
  const scheduleItems = document.getElementById('scheduleItems');
  if (schedCount) schedCount.textContent = '-';
  if (scheduleItems) {
    scheduleItems.innerHTML = '<div class="sched-empty">Tidak terhubung ke ESP32 (-)</div>';
  }

  updateGantryPosition(null, null, '-');
}

// ====== Render UI Saat Terhubung ke ESP32 ======
function renderConnectedUI() {
  const valTemp = document.getElementById('valTemp');
  const barTemp = document.getElementById('barTemp');
  if (valTemp) {
    valTemp.textContent = state.temp !== null ? state.temp.toFixed(1) : '-';
  }
  if (barTemp) {
    barTemp.style.width = state.temp !== null ? `${Math.min(100, Math.max(0, (state.temp / 50) * 100))}%` : '0%';
  }

  const valHum = document.getElementById('valHum');
  const barHum = document.getElementById('barHum');
  if (valHum) {
    valHum.textContent = state.humidity !== null ? Math.round(state.humidity) : '-';
  }
  if (barHum) {
    barHum.style.width = state.humidity !== null ? `${Math.min(100, Math.max(0, state.humidity))}%` : '0%';
  }

  const valSoilA = document.getElementById('valSoilA');
  const valSoilB = document.getElementById('valSoilB');
  const adcA = document.getElementById('adcValueA');
  const adcB = document.getElementById('adcValueB');

  if (valSoilA) {
    if (state.soilA && state.soilA !== '---') {
      valSoilA.textContent = state.soilA;
      valSoilA.className = `badge-soil badge-${state.soilA.toLowerCase()}`;
    } else {
      valSoilA.textContent = '-';
      valSoilA.className = 'badge-soil badge-offline';
    }
  }

  if (valSoilB) {
    if (state.soilB && state.soilB !== '---') {
      valSoilB.textContent = state.soilB;
      valSoilB.className = `badge-soil badge-${state.soilB.toLowerCase()}`;
    } else {
      valSoilB.textContent = '-';
      valSoilB.className = 'badge-soil badge-offline';
    }
  }

  if (adcA) {
    adcA.textContent = state.adcA !== null ? `Nilai ADC: ~${state.adcA} (24 Sel)` : 'Nilai ADC: -';
  }
  if (adcB) {
    adcB.textContent = state.adcB !== null ? `Nilai ADC: ~${state.adcB} (24 Sel)` : 'Nilai ADC: -';
  }

  const statusPump = document.getElementById('statusPump');
  const statusMotion = document.getElementById('statusMotion');
  const driverStatus = document.getElementById('driverStatus');
  const coordX = document.getElementById('coordX');
  const coordY = document.getElementById('coordY');
  const estopAlert = document.getElementById('estopAlert');

  if (statusPump) {
    statusPump.textContent = state.isScanning ? 'AKTIF (PWM 100%)' : 'OFF';
    statusPump.style.color = state.isScanning ? '#16a34a' : 'inherit';
  }

  if (statusMotion) {
    statusMotion.textContent = state.isMoving ? 'BERGERAK...' : (state.isScanning ? 'SCANNING...' : 'IDLE');
    statusMotion.style.color = state.isMoving || state.isScanning ? '#2563eb' : 'inherit';
  }

  if (driverStatus) {
    driverStatus.textContent = state.emergencyStop ? 'Driver: Disabled' : 'Driver: Active';
    driverStatus.style.color = state.emergencyStop ? '#dc2626' : '#64748b';
  }

  if (coordX) {
    coordX.textContent = state.posX !== null ? `${state.posX.toFixed(1)} steps` : '-';
  }
  if (coordY) {
    coordY.textContent = state.posY !== null ? `${state.posY.toFixed(1)} steps` : '-';
  }

  if (estopAlert) {
    estopAlert.style.display = state.emergencyStop ? 'block' : 'none';
  }

  const clockTime = document.getElementById('clockTime');
  const clockDate = document.getElementById('clockDate');
  if (clockTime) clockTime.textContent = state.rtcTime || '-';
  if (clockDate) clockDate.textContent = '-';

  const modeBadge = document.getElementById('modeBadge');
  const connText = document.getElementById('connText');
  const connDot = document.getElementById('connDot');
  const lastUpdated = document.getElementById('lastUpdated');

  if (modeBadge) {
    modeBadge.textContent = 'ESP32: Online';
    modeBadge.className = 'status-badge live';
  }
  if (connText) connText.textContent = 'Terhubung';
  if (connDot) connDot.className = 'dot-indicator';
  if (lastUpdated) {
    lastUpdated.textContent = `Sinkronisasi Cloud: ${state.rtcTime || new Date().toLocaleTimeString()}`;
  }

  updateGantryPosition(state.posX, state.posY, state.selectedCell || (state.isScanning ? 'SCAN' : '-'));
  updateVirtualLCD();
  renderSchedules();
}

// ====== Update Virtual LCD 16x2 (hd44780) ======
function updateVirtualLCD() {
  const line1 = document.getElementById('lcdLine1');
  const line2 = document.getElementById('lcdLine2');
  const modeInd = document.getElementById('lcdModeIndicator');

  if (!line1 || !line2) return;

  if (!state.isConnected) {
    line1.textContent = '-';
    line2.textContent = '-';
    if (modeInd) modeInd.textContent = 'Status: Terputus (-)';
    return;
  }

  if (Date.now() - state.lastLcdRotate > state.lcdInterval) {
    state.lastLcdRotate = Date.now();
    state.lcdMode = (state.lcdMode + 1) % 3;
  }

  if (state.lcdMode === 0) {
    line1.textContent = `Waktu:   ${state.rtcTime || '-'}`;
    line2.textContent = 'Tanggal: -';
    if (modeInd) modeInd.textContent = 'Mode 1/3: Waktu RTC';
  } else if (state.lcdMode === 1) {
    line1.textContent = `Suhu :   ${state.temp !== null ? state.temp.toFixed(1) + ' C' : '-'}`;
    line2.textContent = `Kelembapan:${state.humidity !== null ? Math.round(state.humidity) + '%' : '-'}`;
    if (modeInd) modeInd.textContent = 'Mode 2/3: Suhu & Kelembapan';
  } else if (state.lcdMode === 2) {
    line1.textContent = `ZONA A:  ${state.soilA || '-'}`;
    line2.textContent = `ZONA B:  ${state.soilB || '-'}`;
    if (modeInd) modeInd.textContent = 'Mode 3/3: Zona Kelembapan Tanah';
  }
}

// ====== Render Daftar Jadwal ======
function renderSchedules() {
  const container = document.getElementById('scheduleItems');
  const schedCount = document.getElementById('schedCount');
  if (!container) return;

  if (!state.isConnected) {
    if (schedCount) schedCount.textContent = '-';
    container.innerHTML = '<div class="sched-empty">Tidak terhubung ke ESP32 (-)</div>';
    return;
  }

  if (schedCount) schedCount.textContent = state.schedules.length;

  if (state.schedules.length === 0) {
    container.innerHTML = `
      <div class="sched-empty">
        Tidak ada jadwal yang aktif di memori ESP32.
      </div>
    `;
    return;
  }

  container.innerHTML = '';
  state.schedules.forEach(item => {
    const row = document.createElement('div');
    row.className = 'sched-row';

    row.innerHTML = `
      <div>
        <span class="sched-time-badge">${item.time || '-'}</span>
        <span class="sched-target-badge">${item.target || item.type || 'SCAN'}</span>
      </div>
      <button class="btn btn-sm btn-outline text-danger">Hapus</button>
    `;

    row.querySelector('button').onclick = () => {
      deleteSchedule(item.time, item.type || item.target);
    };

    container.appendChild(row);
  });
}

// ====== Polling Data Real-Time dari Cloud Endpoint (/api/sics) ======
async function fetchLiveStatus() {
  if (state.isFetching) return;
  state.isFetching = true;

  const url = resolveApiUrl();

  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 4000);

    const res = await fetch(url, {
      method: 'GET',
      signal: controller.signal,
      cache: 'no-store',
      headers: { 'Accept': 'application/json' }
    });
    clearTimeout(timeoutId);

    if (!res.ok) {
      throw new Error(`HTTP ${res.status}`);
    }

    const data = await res.json();

    if (data && data.connected) {
      onStatusSuccess(data);
    } else if (data && data.lastSeen) {
      // Ada data telemetri tersimpan di cloud
      onStatusSuccess(data);
    } else {
      onStatusDisconnected('ESP32 Offline (Menunggu data awal)');
    }
  } catch (err) {
    onStatusDisconnected(err.message);
  } finally {
    state.isFetching = false;
  }
}

function onStatusSuccess(data) {
  const justConnected = !state.isConnected;
  state.isConnected = true;
  state.offlineCount = 0;

  if (data.temp !== undefined && data.temp !== null && !isNaN(data.temp)) state.temp = Number(data.temp);
  if (data.humidity !== undefined && data.humidity !== null && !isNaN(data.humidity)) state.humidity = Number(data.humidity);
  if (data.soilA && data.soilA !== '---') state.soilA = data.soilA;
  if (data.soilB && data.soilB !== '---') state.soilB = data.soilB;
  if (data.adcA !== undefined && data.adcA !== null) state.adcA = data.adcA;
  if (data.adcB !== undefined && data.adcB !== null) state.adcB = data.adcB;
  if (data.rtcTime) state.rtcTime = data.rtcTime;
  if (data.posX !== undefined && data.posX !== null) state.posX = Number(data.posX);
  if (data.posY !== undefined && data.posY !== null) state.posY = Number(data.posY);
  state.isMoving = !!data.isMoving;
  state.isScanning = !!data.isScanning;
  state.emergencyStop = !!data.emergencyStop;
  if (Array.isArray(data.schedules)) state.schedules = data.schedules;

  renderConnectedUI();

  if (justConnected && !state.hasEverConnected) {
    state.hasEverConnected = true;
    logEvent('✅ Terhubung ke ESP32 melalui Cloud Server (polmedresearchcenter.id)', 'success');
    showToast('✅ ESP32 Terhubung ke Cloud Server!', 'success');
  }
}

function onStatusDisconnected(reason) {
  state.offlineCount = (state.offlineCount || 0) + 1;

  // Toleransi tinggi: hanya ubah status jika gagal 8x berturut-turut (~16 detik)
  // Data sensor & gantry TETAP UTUH di layar dan TIDAK PERNAH hilang / kedap-kedip.
  if (state.offlineCount < 8 && state.hasEverConnected) {
    const lastUpdated = document.getElementById('lastUpdated');
    if (lastUpdated) {
      lastUpdated.textContent = 'Sinkronisasi Cloud: Menyambungkan kembali...';
    }
    return;
  }

  const wasConnected = state.isConnected;
  state.isConnected = false;

  if (state.hasEverConnected) {
    // Pertahankan angka dan posisi gantry terakhir, hanya ubah badge secara halus
    const modeBadge = document.getElementById('modeBadge');
    const connText = document.getElementById('connText');
    const connDot = document.getElementById('connDot');
    const lastUpdated = document.getElementById('lastUpdated');

    if (modeBadge) {
      modeBadge.textContent = 'ESP32: Menghubungkan';
      modeBadge.className = 'status-badge offline';
    }
    if (connText) connText.textContent = 'Menyambungkan Ulang';
    if (connDot) connDot.className = 'dot-indicator offline';
    if (lastUpdated) lastUpdated.textContent = `Sinkronisasi Terakhir: ${state.rtcTime || '-'}`;
  } else {
    // Belum pernah menerima data sejak website pertama kali dibuka
    renderDisconnectedUI();
  }

  if (wasConnected && state.offlineCount === 8) {
    logEvent(`[Jaringan] Menghubungkan ulang ke cloud... (${reason})`, 'normal');
  }
}

// ====== Eksekusi Perintah ke Cloud (/api/sics) ======
async function doCommand(cmd, extraParams = {}) {
  cmd = cmd.trim();

  if (!state.isConnected && !state.hasEverConnected) {
    showToast('Gagal: ESP32 belum terhubung (-)', 'error');
    logEvent(`[ERROR] Perintah '${cmd}' ditolak: ESP32 belum terhubung (-)`, 'error');
    return;
  }

  if (state.emergencyStop && cmd !== 'RESET') {
    showToast('EMERGENCY STOP AKTIF! Jalankan Reset Sistem terlebih dahulu.', 'error');
    logEvent(`[Ditolak] Perintah ${cmd} diblokir karena Emergency Stop aktif`, 'error');
    return;
  }

  logEvent(`Mengirim perintah ke cloud: ${cmd}...`);

  try {
    const payload = {
      action: 'command',
      cmd: cmd,
      cell: extraParams.cell || ''
    };

    const res = await fetch(resolveApiUrl(), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });

    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();

    logEvent(`Respon Cloud: ${data.message || 'Perintah tersimpan di antrean ESP32'}`, 'success');
    showToast(data.message || 'Perintah dikirim ke ESP32', 'success');

    setTimeout(fetchLiveStatus, 500);
  } catch (e) {
    logEvent(`Gagal mengirim perintah: ${e.message}`, 'error');
    showToast(`Gagal: ${e.message}`, 'error');
  }
}

// ====== Manajemen Jadwal ESP32 ======
async function handleAddSchedule() {
  if (!state.isConnected) {
    showToast('Gagal: ESP32 tidak terhubung (-)', 'error');
    logEvent('[ERROR] Tambah jadwal ditolak: ESP32 tidak terhubung (-)', 'error');
    return;
  }

  const timeVal = document.getElementById('schedTime').value;
  const typeSelect = document.getElementById('schedType');
  const subSelect = document.getElementById('schedSub');

  if (!timeVal || !/^\d{2}:\d{2}$/.test(timeVal)) {
    showToast('Format waktu salah (gunakan HH:MM, contoh: 07:00)', 'error');
    return;
  }

  let type = typeSelect.value;
  let target = type;

  if (type === 'ROW') {
    target = `ROW_${subSelect.value || 'A'}`;
  } else if (type === 'COL') {
    target = `COL_${subSelect.value || '1'}`;
  }

  const scheduleCmd = `SCHEDULE ${timeVal} ${target}`;
  doCommand(scheduleCmd);
}

async function deleteSchedule(time, type) {
  if (!state.isConnected) {
    showToast('Gagal: ESP32 tidak terhubung (-)', 'error');
    return;
  }

  if (!confirm(`Hapus jadwal ${time} (${type}) dari ESP32?`)) return;

  const delCmd = `DELSCHEDULE ${time} ${type || 'SCAN'}`;
  doCommand(delCmd);
}

async function clearAllSchedules() {
  if (!state.isConnected) {
    showToast('Gagal: ESP32 tidak terhubung (-)', 'error');
    return;
  }

  if (!confirm('Hapus seluruh jadwal irigasi dari memori ESP32?')) return;

  doCommand('CLEARSCHEDULES');
}

// ====== Perintah Bot Telegram ======
function handleTelegramCommand(inputVal) {
  if (!inputVal) return;
  inputVal = inputVal.trim();
  const upper = inputVal.toUpperCase();

  logEvent(`[Telegram CMD] ${inputVal}`);

  if (!state.isConnected && !state.hasEverConnected) {
    showToast('Gagal: ESP32 belum terhubung (-)', 'error');
    logEvent('[ERROR] Telegram command dibatalkan: ESP32 belum terhubung (-)', 'error');
    return;
  }

  if (/^(\/)?([A-H])([1-6])$/i.test(upper)) {
    const m = upper.match(/^(\/)?([A-H])([1-6])$/i);
    const cell = `${m[2]}${m[3]}`;
    doCommand('GOTO', { cell });
    return;
  }

  let clean = upper.startsWith('/') ? upper.substring(1) : upper;
  if (clean === 'ZONA_A') clean = 'ZONA A';
  if (clean === 'ZONA_B') clean = 'ZONA B';
  if (clean === 'STOP') clean = 'ES';

  doCommand(clean);
}

// ====== Setup Koneksi & URL Endpoint ======
function setupConnectionHandlers() {
  const inputEspHost = document.getElementById('inputEspHost');
  const btnConnectEsp = document.getElementById('btnConnectEsp');
  const inputIp = document.getElementById('inputIp');
  const modal = document.getElementById('modalConfig');
  const btnToggleConn = document.getElementById('btnToggleConn');
  const btnCloseModal = document.getElementById('btnCloseModal');
  const btnCancelModal = document.getElementById('btnCancelModal');
  const btnSaveConfig = document.getElementById('btnSaveConfig');
  const btnPing = document.getElementById('btnPing');

  const currentApi = resolveApiUrl();
  if (inputEspHost) inputEspHost.value = currentApi;
  if (inputIp) inputIp.value = currentApi;

  function applyEndpoint(newUrl) {
    newUrl = (newUrl || '').trim();
    if (newUrl) {
      localStorage.setItem('sics_custom_api', newUrl);
    } else {
      localStorage.removeItem('sics_custom_api');
    }
    const resolved = resolveApiUrl();
    if (inputEspHost) inputEspHost.value = resolved;
    if (inputIp) inputIp.value = resolved;

    showToast('Menghubungkan ke Endpoint Cloud...');
    logEvent(`Endpoint API diatur ke: ${resolved}`);
    fetchLiveStatus();
  }

  if (btnConnectEsp && inputEspHost) {
    btnConnectEsp.onclick = () => applyEndpoint(inputEspHost.value);
    inputEspHost.onkeydown = (e) => {
      if (e.key === 'Enter') applyEndpoint(inputEspHost.value);
    };
  }

  if (btnToggleConn && modal) {
    btnToggleConn.onclick = () => { modal.style.display = 'flex'; };
  }
  if (btnCloseModal && modal) {
    btnCloseModal.onclick = () => { modal.style.display = 'none'; };
  }
  if (btnCancelModal && modal) {
    btnCancelModal.onclick = () => { modal.style.display = 'none'; };
  }

  if (btnSaveConfig && inputIp && modal) {
    btnSaveConfig.onclick = () => {
      applyEndpoint(inputIp.value);
      modal.style.display = 'none';
    };
  }

  if (btnPing && inputIp) {
    btnPing.onclick = () => {
      const pingUrl = inputIp.value.trim() || resolveApiUrl();
      showToast('Menghubungi Cloud Endpoint...');
      fetch(pingUrl, { signal: AbortSignal.timeout(3500) })
        .then(r => r.json())
        .then(d => {
          if (d.connected) {
            showToast('✅ Berhasil terhubung ke ESP32 via Cloud!', 'success');
          } else {
            showToast('⚠️ Endpoint aktif, namun ESP32 belum online (-)', 'info');
          }
        })
        .catch(e => {
          showToast(`❌ Gagal menghubungi endpoint (${e.message})`, 'error');
        });
    };
  }
}

// ====== Event Listeners Setup ======
function setupEventListeners() {
  const btnEstop = document.getElementById('btnEstop');
  const btnResetEstop = document.getElementById('btnResetEstop');
  const btnReset = document.getElementById('btnReset');
  const btnHoming = document.getElementById('btnHoming');

  if (btnEstop) btnEstop.onclick = () => {
    if (confirm('KONFIRMASI: Aktifkan EMERGENCY STOP sekarang?')) {
      doCommand('ES');
    }
  };
  if (btnResetEstop) btnResetEstop.onclick = () => doCommand('RESET');
  if (btnReset) btnReset.onclick = () => doCommand('RESET');
  if (btnHoming) btnHoming.onclick = () => doCommand('HOME');

  const btnFullScan = document.getElementById('btnFullScan');
  const btnScanZonaA = document.getElementById('btnScanZonaA');
  const btnScanZonaB = document.getElementById('btnScanZonaB');

  if (btnFullScan) btnFullScan.onclick = () => doCommand('SCAN');
  if (btnScanZonaA) btnScanZonaA.onclick = () => doCommand('ZONA A');
  if (btnScanZonaB) btnScanZonaB.onclick = () => doCommand('ZONA B');

  const btnWaterThis = document.getElementById('btnWaterThis');
  if (btnWaterThis) {
    btnWaterThis.onclick = () => {
      if (state.selectedCell) {
        doCommand('GOTO', { cell: state.selectedCell });
      } else {
        showToast('Pilih salah satu sel pada matriks grid terlebih dahulu', 'info');
      }
    };
  }

  const btnGoto = document.getElementById('btnExecuteGoto');
  const inputGoto = document.getElementById('inputGoto');
  if (btnGoto && inputGoto) {
    const handleGoto = () => {
      const code = inputGoto.value.trim().toUpperCase();
      if (/^[A-H][1-6]$/.test(code)) {
        selectCell(code);
        doCommand('GOTO', { cell: code });
      } else {
        showToast('Format salah! Contoh: C3, H1, A6', 'error');
      }
    };
    btnGoto.onclick = handleGoto;
    inputGoto.onkeydown = (e) => {
      if (e.key === 'Enter') handleGoto();
    };
  }

  const jogXPlus = document.getElementById('jogXPlus');
  const jogXMinus = document.getElementById('jogXMinus');
  const jogYPlus = document.getElementById('jogYPlus');
  const jogYMinus = document.getElementById('jogYMinus');
  const jogOrigin = document.getElementById('jogOrigin');

  if (jogOrigin) jogOrigin.onclick = () => doCommand('HOME');
  if (jogXPlus) jogXPlus.onclick = () => doCommand('JOG_X+');
  if (jogXMinus) jogXMinus.onclick = () => doCommand('JOG_X-');
  if (jogYPlus) jogYPlus.onclick = () => doCommand('JOG_Y+');
  if (jogYMinus) jogYMinus.onclick = () => doCommand('JOG_Y-');

  const btnAddSchedule = document.getElementById('btnAddSchedule');
  const btnClearSchedules = document.getElementById('btnClearSchedules');
  if (btnAddSchedule) btnAddSchedule.onclick = handleAddSchedule;
  if (btnClearSchedules) btnClearSchedules.onclick = clearAllSchedules;

  const schedType = document.getElementById('schedType');
  const schedSubWrap = document.getElementById('schedSubWrap');
  const schedSubLabel = document.getElementById('schedSubLabel');
  const schedSub = document.getElementById('schedSub');

  if (schedType && schedSubWrap) {
    schedType.onchange = () => {
      const val = schedType.value;
      if (val === 'ROW') {
        schedSubWrap.style.display = 'flex';
        schedSubLabel.textContent = 'Pilih Baris:';
        schedSub.innerHTML = '';
        for (let r = 0; r < ROWS; r++) {
          const char = String.fromCharCode(65 + r);
          schedSub.add(new Option(`Baris ${char}`, char));
        }
      } else if (val === 'COL') {
        schedSubWrap.style.display = 'flex';
        schedSubLabel.textContent = 'Pilih Kolom:';
        schedSub.innerHTML = '';
        for (let c = 1; c <= COLS; c++) {
          schedSub.add(new Option(`Kolom ${c}`, c));
        }
      } else {
        schedSubWrap.style.display = 'none';
      }
    };
  }

  const btnNextLcd = document.getElementById('btnNextLcd');
  if (btnNextLcd) {
    btnNextLcd.onclick = () => {
      state.lcdMode = (state.lcdMode + 1) % 3;
      state.lastLcdRotate = Date.now();
      updateVirtualLCD();
    };
  }

  const btnSendTg = document.getElementById('btnSendTg');
  const inputTg = document.getElementById('inputTg');
  if (btnSendTg && inputTg) {
    btnSendTg.onclick = () => {
      handleTelegramCommand(inputTg.value);
      inputTg.value = '';
    };
    inputTg.onkeydown = (e) => {
      if (e.key === 'Enter') {
        handleTelegramCommand(inputTg.value);
        inputTg.value = '';
      }
    };
  }

  document.querySelectorAll('.chip').forEach(chip => {
    chip.onclick = () => handleTelegramCommand(chip.dataset.cmd);
  });

  const btnClearLog = document.getElementById('btnClearLog');
  const btnExportLog = document.getElementById('btnExportLog');
  if (btnClearLog) {
    btnClearLog.onclick = () => {
      const box = document.getElementById('terminalLogBox');
      if (box) box.innerHTML = '';
    };
  }
  if (btnExportLog) {
    btnExportLog.onclick = () => {
      const box = document.getElementById('terminalLogBox');
      const text = box ? box.innerText : '';
      const blob = new Blob([text], { type: 'text/plain' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = `sics_log_${new Date().toISOString().slice(0, 10)}.txt`;
      a.click();
    };
  }
}

// ====== Inisialisasi Saat Halaman Dimuat ======
window.addEventListener('DOMContentLoaded', () => {
  initGridMatrix();
  initRowColButtons();
  setupConnectionHandlers();
  setupEventListeners();

  renderDisconnectedUI();

  logEvent('=== SICS Gantry Watering V2.0 Dimuat ===');
  logEvent('Sistem: Cloud Architecture (polmedresearchcenter.id/api/sics)');

  // Loop polling cerdas & adaptif:
  // - 1000ms saat mencari sinyal ESP32 agar langsung terhubung saat ESP32 baru dinyalakan
  // - 2000ms saat sudah terhubung agar stabil, mulus, dan tidak membebani server
  async function runPollingLoop() {
    await fetchLiveStatus();
    const interval = state.isConnected ? 2000 : 1000;
    setTimeout(runPollingLoop, interval);
  }
  runPollingLoop();

  setInterval(updateVirtualLCD, 1000);
});
