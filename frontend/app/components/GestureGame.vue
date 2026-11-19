<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'

type Point = { t: number; gesture: number; name: string; confidence: number }
const props = defineProps<{ history: Point[] }>()

// Slow, two-state flappy: extension steers the bird up, flexion steers it down.
// The bird only moves while a gesture has been seen; it hovers until the first one.
const W = 640
const H = 400
const BIRD_X = 150
const BIRD_R = 12
const PIPE_W = 56
const GAP_START = 180
const GAP_MIN = 110
const GAP_STEP = 10       // each 5 pipes the gap shrinks by this much
const GAP_EVERY = 5
const SPACING = 400
const SPEED = 52          // horizontal px/s (~8 s between pipes)
const VY = 28             // vertical px/s (~70 px per gesture, fits the 180 px gap)
const BEST_KEY = 'omniflux-flappy-best'

const canvas = ref<HTMLCanvasElement>()
const running = ref(false)
const over = ref(false)
const score = ref(0)
const best = ref(0)
const recording = computed(() => props.history.at(-1)?.name === 'MOVING')

// Plain state for the render loop.
let ctx: CanvasRenderingContext2D | null = null
let raf = 0
let lastFrame = 0
let birdY = H / 2
let dir = 0              // -1 up, +1 down, 0 waiting for a gesture
let pipes: { x: number; gapY: number; gap: number; scored: boolean }[] = []
let lastT = 0

function gapFor(passed: number) {
  return Math.max(GAP_MIN, GAP_START - Math.floor(passed / GAP_EVERY) * GAP_STEP)
}

function spawn(x: number) {
  const gap = gapFor(score.value)
  const edge = gap / 2 + 48
  pipes.push({ x, gapY: edge + Math.random() * (H - 2 * edge), gap, scored: false })
}

function reset() {
  birdY = H / 2
  dir = 0
  pipes = []
  score.value = 0
  spawn(W - 80)
  spawn(W - 80 + SPACING)
}

function start() {
  const el = canvas.value
  if (!el) return
  ctx = el.getContext('2d')
  // Snapshots replay the whole history on reconnect: ignore everything up to now.
  lastT = props.history.at(-1)?.t ?? 0
  reset()
  over.value = false
  running.value = true
  lastFrame = performance.now()
  cancelAnimationFrame(raf)
  raf = requestAnimationFrame(frame)
}

function end() {
  running.value = false
  over.value = true
  if (score.value > best.value) {
    best.value = score.value
    localStorage.setItem(BEST_KEY, String(best.value))
  }
}

function frame(now: number) {
  const dt = Math.min(0.05, (now - lastFrame) / 1000)
  lastFrame = now
  update(dt)
  if (!running.value) return
  draw()
  raf = requestAnimationFrame(frame)
}

function update(dt: number) {
  birdY += dir * VY * dt
  if (birdY - BIRD_R < 0 || birdY + BIRD_R > H) return end()

  for (const p of pipes) p.x -= SPEED * dt

  const last = pipes[pipes.length - 1]
  if (last && last.x < W - SPACING) spawn(last.x + SPACING)

  for (const p of pipes) {
    const overlapX = BIRD_X + BIRD_R > p.x && BIRD_X - BIRD_R < p.x + PIPE_W
    if (overlapX && (birdY - BIRD_R < p.gapY - p.gap / 2 || birdY + BIRD_R > p.gapY + p.gap / 2)) {
      return end()
    }
    if (!p.scored && p.x + PIPE_W < BIRD_X - BIRD_R) {
      p.scored = true
      score.value++
    }
  }
  pipes = pipes.filter((p) => p.x + PIPE_W > -10)
}

function draw() {
  if (!ctx) return
  ctx.clearRect(0, 0, W, H)
  ctx.fillStyle = '#0f1115'
  ctx.fillRect(0, 0, W, H)

  ctx.lineWidth = 2
  for (const p of pipes) {
    const top = p.gapY - p.gap / 2
    const bottom = p.gapY + p.gap / 2
    for (const [y, h] of [[0, top], [bottom, H - bottom]] as const) {
      ctx.fillStyle = '#1f2430'
      ctx.strokeStyle = '#3b4252'
      ctx.beginPath()
      ctx.roundRect(p.x, y, PIPE_W, h, 6)
      ctx.fill()
      ctx.stroke()
    }
  }

  ctx.beginPath()
  ctx.arc(BIRD_X, birdY, BIRD_R, 0, Math.PI * 2)
  ctx.fillStyle = dir < 0 ? '#34d399' : dir > 0 ? '#60a5fa' : '#94a3b8'
  ctx.fill()
  ctx.beginPath()
  ctx.arc(BIRD_X + 4, birdY - 4, 2.5, 0, Math.PI * 2)
  ctx.fillStyle = '#0f1115'
  ctx.fill()
}

watch(
  () => props.history,
  (h) => {
    const p = h.at(-1)
    if (!p || p.gesture < 0 || p.t <= lastT) return
    lastT = p.t
    if (!running.value) return
    if (p.name === 'WRIST_EXTENSION') dir = -1
    else if (p.name === 'WRIST_FLEXION') dir = 1
  },
)

function key(event: KeyboardEvent) {
  if (!running.value) return
  if (event.key === 'ArrowUp') dir = -1
  else if (event.key === 'ArrowDown') dir = 1
}

onMounted(() => {
  best.value = Number(localStorage.getItem(BEST_KEY) ?? 0)
  window.addEventListener('keydown', key)
})
onBeforeUnmount(() => {
  cancelAnimationFrame(raf)
  window.removeEventListener('keydown', key)
})
</script>

<template>
  <section class="panel">
    <div class="head">
      <h2>Wrist Flappy</h2>
      <span class="score">{{ score }}</span>
    </div>

    <div class="stage">
      <canvas ref="canvas" width="640" height="400" role="img" aria-label="Slow flappy bird controlled with wrist gestures" />
      <div v-if="!running" class="overlay">
        <button type="button" @click="start">{{ over ? 'Play again' : 'Play' }}</button>
        <small v-if="best">Best: {{ best }}</small>
      </div>
    </div>

    <div class="hint">
      <span>{{ recording ? 'Recording…' : 'Extension ↑ · Flexion ↓' }}</span>
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
  max-width: 340px;
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
