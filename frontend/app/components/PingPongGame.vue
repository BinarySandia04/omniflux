<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue'

type Point = { t: number; player?: number; gesture: number; name: string; confidence: number }
type Player = { id: number; name: string; connected: boolean }
const props = defineProps<{ history: Point[]; players: Player[] }>()

// Two players, one board each. A ball bounces between the paddles; the player it
// reaches must flick extension -> flexion within 1 s of arrival. The ESP32
// reports each gesture after a ~2 s capture window, so the ball waits at the
// paddle for the verdict instead of deciding the moment it arrives.
const W = 640
const H = 400
const PADDLE_W = 14
const PADDLE_H = 84
const PADDLE_X = 48
const BALL_R = 9
const BASE_SPEED = 190      // px/s (~2.7 s across the court)
const HIT_WINDOW_MS = 1000  // flick must be within 1 s before the ball arrives
const HIT_GRACE_MS = 250    // small grace after arrival
const INFERENCE_DELAY_MS = 2000 // ESP32 capture + classification delay
const VERDICT_MS = INFERENCE_DELAY_MS + HIT_GRACE_MS + 250
const SERVE_DELAY_MS = 900
const FLASH_MS = 450

const canvas = ref<HTMLCanvasElement>()
const running = ref(false)
const over = ref(false)
const score = ref({ 1: 0, 2: 0 })

const bothConnected = computed(() =>
  [1, 2].every((id) => props.players.some((p) => p.id === id && p.connected)),
)

let ctx: CanvasRenderingContext2D | null = null
let raf = 0
let lastFrame = 0
let lastT = 0
let phase: 'serving' | 'flying' | 'judging' = 'flying'
let serveAt = 0
let serveDir: 1 | -1 = 1
let ball = { x: W / 2, y: H / 2, vx: BASE_SPEED, vy: 0 }
let judge: { side: 1 | 2; arrivalT: number; deadline: number } | null = null
let flicks: Record<1 | 2, number> = { 1: -Infinity, 2: -Infinity }
let prevGesture: Record<1 | 2, string> = { 1: '', 2: '' }
let flash: Record<1 | 2, number> = { 1: 0, 2: 0 }
let flashKind: Record<1 | 2, 'hit' | 'miss' | ''> = { 1: '', 2: '' }

function reset() {
  score.value = { 1: 0, 2: 0 }
  flicks = { 1: -Infinity, 2: -Infinity }
  prevGesture = { 1: '', 2: '' }
  flash = { 1: 0, 2: 0 }
  flashKind = { 1: '', 2: '' }
  judge = null
  serveDir = Math.random() < 0.5 ? -1 : 1
  phase = 'serving'
  serveAt = Date.now() + 600
}

function serve(dir: 1 | -1) {
  // Start at the serving paddle so the receiver gets the whole court's travel
  // time (~2.7 s) to react before the ball arrives.
  ball = {
    x: dir > 0 ? PADDLE_X + PADDLE_W + BALL_R + 4 : W - PADDLE_X - PADDLE_W - BALL_R - 4,
    y: H / 2 + (Math.random() - 0.5) * 140,
    vx: dir * BASE_SPEED,
    vy: (Math.random() * 2 - 1) * 100,
  }
  phase = 'flying'
}

function start() {
  const el = canvas.value
  if (!el) return
  ctx = el.getContext('2d')
  // Snapshots replay the whole history on reconnect: ignore everything up to now.
  lastT = props.history.at(-1)?.t ?? 0
  over.value = false
  running.value = true
  reset()
  lastFrame = performance.now()
  cancelAnimationFrame(raf)
  raf = requestAnimationFrame(frame)
}

function end() {
  running.value = false
  over.value = true
}

function frame(now: number) {
  const dt = Math.min(0.05, (now - lastFrame) / 1000)
  lastFrame = now
  update(dt, Date.now())
  if (!running.value) return
  draw(Date.now())
  raf = requestAnimationFrame(frame)
}

function arrive(side: 1 | 2, now: number) {
  phase = 'judging'
  judge = { side, arrivalT: now, deadline: now + VERDICT_MS }
  ball.x = side === 1 ? PADDLE_X + PADDLE_W + BALL_R : W - PADDLE_X - PADDLE_W - BALL_R
}

function update(dt: number, now: number) {
  if (phase === 'serving') {
    if (now >= serveAt) serve(serveDir)
    return
  }

  if (phase === 'flying') {
    ball.x += ball.vx * dt
    ball.y += ball.vy * dt

    if (ball.y < BALL_R) {
      ball.y = BALL_R
      ball.vy = Math.abs(ball.vy)
    } else if (ball.y > H - BALL_R) {
      ball.y = H - BALL_R
      ball.vy = -Math.abs(ball.vy)
    }

    if (ball.vx < 0 && ball.x <= PADDLE_X + PADDLE_W + BALL_R) arrive(1, now)
    else if (ball.vx > 0 && ball.x >= W - PADDLE_X - PADDLE_W - BALL_R) arrive(2, now)
    return
  }

  if (phase === 'judging' && judge && now >= judge.deadline) {
    const { side, arrivalT } = judge
    const physicalT = flicks[side]
    const hit = physicalT >= arrivalT - HIT_WINDOW_MS && physicalT <= arrivalT + HIT_GRACE_MS
    flicks[side] = -Infinity

    if (hit) {
      flash[side] = now + FLASH_MS
      flashKind[side] = 'hit'
      ball.vx = side === 1 ? Math.abs(ball.vx) * 1.06 : -Math.abs(ball.vx) * 1.06
      ball.vy = (Math.random() * 2 - 1) * 110
      phase = 'flying'
    } else {
      const scorer = side === 1 ? 2 : 1
      flash[side] = now + FLASH_MS
      flashKind[side] = 'miss'
      score.value[scorer]++
      if (score.value[scorer] >= 7) {
        judge = null
        return end()
      }
      phase = 'serving'
      serveDir = side === 1 ? -1 : 1
      serveAt = now + SERVE_DELAY_MS
    }
    judge = null
  }
}

function paddle(side: 1 | 2, now: number) {
  if (!ctx) return
  const x = side === 1 ? PADDLE_X : W - PADDLE_X - PADDLE_W
  const active = judge?.side === side
  const flashing = flash[side] > now

  ctx.beginPath()
  ctx.roundRect(x, H / 2 - PADDLE_H / 2, PADDLE_W, PADDLE_H, 7)
  ctx.fillStyle = flashing
    ? flashKind[side] === 'hit'
      ? '#34d399'
      : '#f87171'
    : active
      ? '#60a5fa'
      : '#94a3b8'
  ctx.fill()

  if (active) {
    ctx.beginPath()
    ctx.arc(side === 1 ? x + PADDLE_W + 4 : x - 4, ball.y, 14 + Math.sin(now / 120) * 3, 0, Math.PI * 2)
    ctx.strokeStyle = '#60a5fa'
    ctx.lineWidth = 2
    ctx.stroke()
  }
}

function draw(now: number) {
  if (!ctx) return
  ctx.clearRect(0, 0, W, H)
  ctx.fillStyle = '#0f1115'
  ctx.fillRect(0, 0, W, H)

  ctx.strokeStyle = '#1f2430'
  ctx.lineWidth = 2
  ctx.setLineDash([8, 12])
  ctx.beginPath()
  ctx.moveTo(W / 2, 0)
  ctx.lineTo(W / 2, H)
  ctx.stroke()
  ctx.setLineDash([])

  ctx.fillStyle = '#334155'
  ctx.font = '700 56px ui-sans-serif, system-ui, sans-serif'
  ctx.textAlign = 'center'
  ctx.fillText(String(score.value[1]), W / 2 - 70, 72)
  ctx.fillText(String(score.value[2]), W / 2 + 70, 72)

  paddle(1, now)
  paddle(2, now)

  if (phase !== 'serving') {
    ctx.beginPath()
    ctx.arc(ball.x, ball.y, BALL_R, 0, Math.PI * 2)
    ctx.fillStyle = phase === 'judging' ? '#fbbf24' : '#e5e7eb'
    ctx.fill()
  }
}

watch(
  () => props.history,
  (h) => {
    const p = h.at(-1)
    if (!p || p.t <= lastT) return
    lastT = p.t
    if (p.gesture < 0) return   // MOVING / NO_MOVEMENT carry no wrist state

    const side: 1 | 2 = p.player === 2 ? 2 : 1
    if (prevGesture[side] === 'WRIST_EXTENSION' && p.name === 'WRIST_FLEXION') {
      // The gesture was made ~INFERENCE_DELAY_MS before the board reported it.
      flicks[side] = p.t - INFERENCE_DELAY_MS
    }
    prevGesture[side] = p.name
  },
)

onBeforeUnmount(() => cancelAnimationFrame(raf))
</script>

<template>
  <section class="panel">
    <div class="head">
      <h2>Ping Pong</h2>
      <span class="score">
        <span class="p1">P1</span> {{ score[1] }} — {{ score[2] }} <span class="p2">P2</span>
      </span>
    </div>

    <div class="stage">
      <canvas ref="canvas" width="640" height="400" role="img" aria-label="Two-player ping pong controlled with wrist gestures" />

      <div v-if="!bothConnected" class="overlay">
        <p>Waiting for both ESP32 players…</p>
        <small>Connect player 1 and player 2 over Bluetooth.</small>
      </div>

      <div v-else-if="!running" class="overlay">
        <p>
          When the ball reaches your paddle, flick from extension to flexion within
          one second. The board reports the gesture about 2 s later, so the ball
          waits at the paddle while the hit is judged.
        </p>
        <button type="button" @click="start">{{ over ? 'Play again' : 'Play' }}</button>
        <small v-if="over">First to 7 wins.</small>
      </div>
    </div>

    <div class="hint">
      <span>Extension → flexion = hit</span>
      <span>P1 left · P2 right · first to 7</span>
    </div>
  </section>
</template>

<style scoped>
.head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 12px;
}
.head h2 {
  margin: 0;
}
.score {
  font-size: 22px;
  font-weight: 700;
  font-variant-numeric: tabular-nums;
}
.p1 {
  color: #34d399;
}
.p2 {
  color: #60a5fa;
}
.stage {
  position: relative;
}
canvas {
  display: block;
  width: 100%;
  height: auto;
  border-radius: 10px;
  background: #0f1115;
}
.overlay {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 12px;
  padding: 24px;
  text-align: center;
  background: rgba(15, 17, 21, 0.78);
  border-radius: 10px;
}
.overlay p {
  margin: 0;
  max-width: 380px;
  color: #9ca3af;
  font-size: 14px;
  line-height: 1.5;
}
.overlay button {
  padding: 10px 24px;
  border: 0;
  border-radius: 999px;
  background: linear-gradient(90deg, #34d399, #60a5fa);
  color: #0f1115;
  font-size: 15px;
  font-weight: 700;
  cursor: pointer;
}
.overlay button:focus-visible {
  outline: 2px solid #e5e7eb;
  outline-offset: 2px;
}
.overlay small {
  color: #6b7280;
  font-size: 12px;
}
.hint {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  margin-top: 10px;
  color: #6b7280;
  font-size: 12px;
}
@media (max-width: 480px) {
  .hint {
    flex-direction: column;
  }
}
</style>
