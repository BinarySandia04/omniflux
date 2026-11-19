// Reads the JSON inference lines the ESP32 prints on USB serial and streams
// the rolling history + statistics to the web frontend over SSE.
//
//   npm start
//   SERIAL_PORT=/dev/ttyACM0 npm start
//
// Endpoints: GET /api/events (SSE), GET /api/state (JSON snapshot).

import http from 'node:http'
import { SerialPort } from 'serialport'

const PORT = Number(process.env.PORT ?? 3001)
const SERIAL_PATH = process.env.SERIAL_PORT ?? '/dev/ttyACM1'
const BAUD = Number(process.env.SERIAL_BAUD ?? 115200)
const HISTORY_LIMIT = 120

const history = []          // {t, gesture, name, confidence}; negative gesture = status (MOVING, NO_MOVEMENT)
const counts = {}           // name -> number of gestures (idle events excluded)
let total = 0
let sumConfidence = 0

const clients = new Set()

function snapshot() {
  return {
    history,
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

function ingest(line) {
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

  const point = {
    t: Date.now(),
    gesture,
    name: typeof msg.name === 'string' ? msg.name : String(gesture),
    confidence: Math.min(1, Math.max(0, confidence)),
  }

  history.push(point)
  if (history.length > HISTORY_LIMIT) history.shift()

  // Status events (MOVING while recording, NO_MOVEMENT when idle) are shown in
  // the timeline but are not gestures: keep them out of the counts and average.
  if (point.gesture >= 0) {
    total++
    sumConfidence += point.confidence
    counts[point.name] = (counts[point.name] ?? 0) + 1
  }

  publish()
}

let serialBuffer = ''

function connectSerial() {
  const port = new SerialPort({ path: SERIAL_PATH, baudRate: BAUD })
  let retry = null

  const reconnect = () => {
    clearTimeout(retry)
    retry = setTimeout(connectSerial, 2000)
  }

  port.on('open', () => console.log(`Serial: ${SERIAL_PATH} @ ${BAUD}`))
  port.on('data', (chunk) => {
    serialBuffer += chunk.toString()
    let end
    while ((end = serialBuffer.indexOf('\n')) !== -1) {
      const line = serialBuffer.slice(0, end).trim()
      serialBuffer = serialBuffer.slice(end + 1)
      if (line) ingest(line)
    }
  })
  port.on('error', (err) => {
    console.error(`Serial error: ${err.message}`)
    reconnect()
  })
  port.on('close', reconnect)
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
  res.end(`OmniFlux backend\nSerial: ${SERIAL_PATH}\nGestures: ${total}\n`)
})

// SSE comments keep idle connections alive through proxies.
setInterval(() => {
  for (const res of clients) res.write(': keep-alive\n\n')
}, 25000)

server.listen(PORT, () => {
  console.log(`Backend: http://localhost:${PORT}`)
  connectSerial()
})
