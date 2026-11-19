// Reads the JSON inference lines the ESP32 sends (USB serial or Bluetooth LE)
// and streams the rolling history + statistics to the web frontend over SSE.
//
//   npm start                        # USB serial (one port unless SERIAL_PORTS is set)
//   TRANSPORT=ble npm start          # Bluetooth LE, backend connects to the ESP32
//   SERIAL_PORTS=/dev/ttyACM0,/dev/ttyACM1 npm start
//
// Endpoints: GET /api/events (SSE), GET /api/state (JSON snapshot).

import http from 'node:http'
import { SerialPort } from 'serialport'
import { createBluetooth } from 'node-ble'

const PORT = Number(process.env.PORT ?? 3001)
const TRANSPORT = process.env.TRANSPORT ?? 'serial'
const serialEnv = process.env.SERIAL_PORTS ?? process.env.SERIAL_PORT
const SERIAL_PATHS = serialEnv
  ? serialEnv.split(',').map((path) => path.trim()).filter(Boolean)
  : ['/dev/ttyACM1']
const BAUD = Number(process.env.SERIAL_BAUD ?? 115200)
const HISTORY_LIMIT = 120

// Must match the UUIDs and advertised name in Frimware/src/main.cpp.
const BLE_PREFIX = process.env.BLE_NAME ?? 'OmniFlux'
const BLE_SERVICE_UUID = '6e400001-b5a3-f393-e0a9-e50e24dcca9e'
const BLE_TX_UUID = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'
const BLE_RETRY_MS = 3000

const history = []          // {t, player, gesture, name, confidence}; negative gesture = status
const counts = {}           // name -> number of gestures (idle events excluded)
let total = 0
let sumConfidence = 0

const players = new Map()   // player id -> { id, name, connected }
const playerStats = new Map()   // player id -> { total, sumConfidence }
const playerLast = new Map()   // player id -> latest point (status events included)
const clients = new Set()

function snapshot() {
  return {
    history,
    players: [...players.values()]
      .sort((a, b) => a.id - b.id)
      .map((player) => {
        const s = playerStats.get(player.id)
        return {
          ...player,
          last: playerLast.get(player.id) ?? null,
          stats: {
            total: s?.total ?? 0,
            avgConfidence: s?.total ? s.sumConfidence / s.total : 0,
          },
        }
      }),
    stats: {
      total,
      counts,
      avgConfidence: total ? sumConfidence / total : 0,
    },
  }
}

function publish() {
  const frame = `event: snapshot\ndata: ${JSON.stringify(snapshot())}\n\n`
  for (const res of clients) res.write(frame)
}

function ingest(line, fallbackPlayer) {
  const start = line.indexOf('{')
  if (start === -1) return   // debug lines (#WINDOW, boot messages)

  let msg
  try {
    msg = JSON.parse(line.slice(start))
  } catch {
    return
  }

  const gesture = Number(msg.gesture)
  const confidence = Number(msg.confidence)
  if (!Number.isFinite(gesture) || !Number.isFinite(confidence)) return

  // The firmware tags each line with its player; older firmware lacks it, so
  // fall back to the position of the port in SERIAL_PORTS.
  const tagged = Number(msg.player)
  const player = Number.isInteger(tagged) && tagged > 0 ? tagged : fallbackPlayer

  const point = {
    t: Date.now(),
    player,
    gesture,
    name: typeof msg.name === 'string' ? msg.name : String(gesture),
    confidence: Math.min(1, Math.max(0, confidence)),
  }

  history.push(point)
  if (history.length > HISTORY_LIMIT) history.shift()
  playerLast.set(player, point)

  // Status events (MOVING while recording, NO_MOVEMENT when idle) are shown in
  // the timeline but are not gestures: keep them out of the counts and average.
  if (point.gesture >= 0) {
    total++
    sumConfidence += point.confidence
    counts[point.name] = (counts[point.name] ?? 0) + 1

    const s = playerStats.get(player) ?? { total: 0, sumConfidence: 0 }
    playerStats.set(player, {
      total: s.total + 1,
      sumConfidence: s.sumConfidence + point.confidence,
    })
  }

  publish()
  return player
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

function setPlayer(id, name, connected, transport) {
  players.set(id, { id, name, connected, transport })
  publish()
}

// Both transports deliver newline-terminated JSON split over arbitrary chunks
// (BLE notifications are 20 bytes), so every connection gets its own buffer.
function lineReader(fallbackPlayer, onPlayer) {
  let buffer = ''
  let player = fallbackPlayer
  let announced = false
  return (chunk) => {
    buffer += chunk.toString()
    let end
    while ((end = buffer.indexOf('\n')) !== -1) {
      const line = buffer.slice(0, end).trim()
      buffer = buffer.slice(end + 1)
      if (!line) continue
      const used = ingest(line, player)
      if (!used) continue
      if (used !== player || !announced) {
        player = used
        announced = true
        onPlayer?.(player)   // follow the firmware's player tag
      }
    }
  }
}

function connectSerial(path, fallbackPlayer) {
  const port = new SerialPort({ path, baudRate: BAUD })
  let player = fallbackPlayer
  let retry = null

  const reconnect = () => {
    clearTimeout(retry)
    retry = setTimeout(() => connectSerial(path, fallbackPlayer), 2000)
  }

  // Wait for the first line before claiming a player: the USB port order is
  // not guaranteed, and the firmware tags every line with its own id.
  const read = lineReader(fallbackPlayer, (tagged) => {
    player = tagged
    setPlayer(player, 'serial', true, 'serial')
  })

  port.on('open', () => console.log(`Serial: ${path} @ ${BAUD}`))
  port.on('data', read)
  port.on('error', (err) => {
    console.error(`Serial ${path}: ${err.message}`)
    setPlayer(player, 'serial', false, 'serial')
    reconnect()
  })
  port.on('close', () => {
    setPlayer(player, 'serial', false, 'serial')
    reconnect()
  })
}

// "OmniFlux-2" -> player 2; a bare "OmniFlux" is player 1.
function playerFromName(name) {
  const match = /-(\d+)$/.exec(name)
  return match ? Number(match[1]) : 1
}

function isPlayerDevice(name) {
  return name === BLE_PREFIX || name.startsWith(`${BLE_PREFIX}-`)
}

const links = new Set()   // BLE addresses that already have a connection task
const names = new Map()   // BLE address -> advertised name, so each device is probed once

async function handleDevice(device, address, name) {
  const player = playerFromName(name)
  let tx
  try {
    // Already connected if the backend restarted while the board kept the link.
    if (!(await device.isConnected())) await device.connect()
    const gatt = await device.gatt()
    const service = await gatt.getPrimaryService(BLE_SERVICE_UUID)
    tx = await service.getCharacteristic(BLE_TX_UUID)
    tx.on('valuechanged', lineReader(player))
    await tx.startNotifications()

    setPlayer(player, name, true, 'ble')
    console.log(`Bluetooth: player ${player} connected (${name} ${address})`)

    // Poll instead of waiting for the 'disconnect' event: after a backend
    // restart the device is already connected, so node-ble's listener was
    // never registered and the event would never fire.
    for (;;) {
      await sleep(2000)
      try {
        if (!(await device.isConnected())) break
      } catch {
        break   // BlueZ forgot the device
      }
    }
    console.log(`Bluetooth: player ${player} disconnected`)
  } catch (err) {
    console.error(`Bluetooth player ${player} (${name}): ${err.message}`)
  } finally {
    // node-ble only drops its DBus signal listeners on disconnect, and that
    // call fails when the device is already gone, so clear them unconditionally.
    // Otherwise every reconnect leaks a match rule until BlueZ refuses new ones.
    await device.disconnect().catch(() => {})
    device.helper.removeListeners()
    tx?.helper.removeListeners()
    setPlayer(player, name, false, 'ble')
    links.delete(address)
  }
}

async function bluetoothLoop() {
  const { bluetooth, destroy } = createBluetooth()
  // dbus-next emits match-rule and connection failures here; without a handler
  // Node would treat it as an unhandled 'error' event and kill the backend.
  bluetooth.dbus.on('error', (err) => console.error(`Bluetooth bus: ${err.message}`))
  try {
    const adapter = await bluetooth.defaultAdapter()
    if (!(await adapter.isDiscovering())) await adapter.startDiscovery()
    console.log(`Bluetooth: scanning for ${BLE_PREFIX}-*`)

    // Keep scanning so a board that reboots or comes back is picked up again.
    for (;;) {
      for (const address of await adapter.devices()) {
        if (links.has(address)) continue
        let name = names.get(address)
        if (name === undefined) {
          let probe
          try {
            probe = await adapter.getDevice(address)
            name = await probe.getName()
          } catch {
            probe?.helper.removeListeners()
            continue   // device disappeared or has no name yet
          }
          // shortcut: node-ble has no public way to release a Device proxy, and
          // its PropertiesChanged listener would pile up on the shared bus.
          probe.helper.removeListeners()
          names.set(address, name)
        }
        if (!isPlayerDevice(name)) continue
        let device
        try {
          device = await adapter.getDevice(address)
        } catch {
          continue   // device disappeared between listing and lookup
        }
        links.add(address)
        handleDevice(device, address, name)
      }
      await sleep(1500)
    }
  } catch (err) {
    console.error(`Bluetooth error: ${err.message}`)
  } finally {
    destroy()
    setTimeout(bluetoothLoop, BLE_RETRY_MS)
  }
}

const server = http.createServer((req, res) => {
  res.setHeader('Access-Control-Allow-Origin', '*')
  const { pathname } = new URL(req.url, 'http://localhost')

  if (pathname === '/api/events') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      Connection: 'keep-alive',
    })
    res.write(`retry: 2000\nevent: snapshot\ndata: ${JSON.stringify(snapshot())}\n\n`)
    clients.add(res)
    req.on('close', () => clients.delete(res))
    return
  }

  if (pathname === '/api/state') {
    res.writeHead(200, { 'Content-Type': 'application/json' })
    res.end(JSON.stringify(snapshot()))
    return
  }

  res.writeHead(200, { 'Content-Type': 'text/plain' })
  res.end(`OmniFlux backend\nTransport: ${TRANSPORT}\nGestures: ${total}\n`)
})

// SSE comments keep idle connections alive through proxies.
setInterval(() => {
  for (const res of clients) res.write(': keep-alive\n\n')
}, 25000)

server.listen(PORT, () => {
  console.log(`Backend: http://localhost:${PORT} (${TRANSPORT})`)
  if (TRANSPORT === 'ble') {
    bluetoothLoop()
  } else {
    SERIAL_PATHS.forEach((path, i) => connectSerial(path, i + 1))
  }
})
