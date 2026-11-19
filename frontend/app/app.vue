<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'

type Point = { t: number; gesture: number; name: string; confidence: number }
type Stats = { total: number; counts: Record<string, number>; avgConfidence: number }

const apiBase = (useRuntimeConfig().public.apiBase as string).replace(/\/$/, '')

const history = ref<Point[]>([])
const stats = ref<Stats>({ total: 0, counts: {}, avgConfidence: 0 })
const connected = ref(false)
let source: EventSource | undefined

onMounted(() => {
  source = new EventSource(`${apiBase}/api/events`)
  // Every snapshot carries the full history + stats, so state is simply replaced.
  source.addEventListener('snapshot', (event) => {
    const data = JSON.parse((event as MessageEvent).data)
    history.value = data.history
    stats.value = data.stats
  })
  source.onopen = () => { connected.value = true }
  source.onerror = () => { connected.value = false }
})

onBeforeUnmount(() => source?.close())

const last = computed(() => history.value.at(-1))
const idle = computed(() => last.value?.name === 'NO_MOVEMENT')
const moving = computed(() => last.value?.name === 'MOVING')
const pct = (value: number) => `${Math.round(value * 100)}%`

const classes = computed(() =>
  Object.entries(stats.value.counts).map(([name, count]) => ({
    name,
    count,
    percent: stats.value.total ? (count / stats.value.total) * 100 : 0,
  })),
)

// Confidence timeline: last 60 gestures, mapped into a 120x40 viewBox.
const chart = computed(() => {
  const points = history.value.filter((p) => p.gesture >= 0).slice(-60)
  const mx = (i: number) =>
    points.length <= 1 ? 60 : (i / (points.length - 1)) * 120
  const my = (v: number) => 36 - v * 32
  return {
    count: points.length,
    line: points.map((p, i) => `${mx(i)},${my(p.confidence)}`).join(' '),
    dots: points.map((p, i) => ({
      cx: mx(i),
      cy: my(p.confidence),
      fill: p.gesture === 1 ? '#34d399' : '#60a5fa',
    })),
  }
})
</script>

<template>
  <main>
    <header>
      <div>
        <h1>OmniFlux</h1>
        <p>Live wrist gesture recognition over USB serial</p>
      </div>
      <span class="badge" :class="connected ? 'on' : 'off'" aria-live="polite">
        {{ connected ? 'Live' : 'Disconnected' }}
      </span>
    </header>

    <section class="cards">
      <article>
        <span>Gestures</span>
        <strong>{{ stats.total }}</strong>
      </article>
      <article>
        <span>Last gesture</span>
        <strong>{{ last ? (moving ? 'Recording…' : idle ? 'No movement' : last.name.replace('_', ' ')) : '—' }}</strong>
        <small v-if="last && !idle && !moving">{{ pct(last.confidence) }} confidence</small>
        <small v-else-if="moving">Gesture in progress</small>
        <small v-else-if="idle">No movement detected</small>
      </article>
      <article>
        <span>Average confidence</span>
        <strong>{{ stats.total ? pct(stats.avgConfidence) : '—' }}</strong>
      </article>
    </section>

    <section class="panel">
      <h2>Wrist motion</h2>
      <svg viewBox="0 0 260 170" class="wrist" role="img" aria-label="Animated wrist extension and flexion">
        <defs>
          <linearGradient id="skin" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stop-color="#d7dee8" />
            <stop offset="1" stop-color="#b9c3d1" />
          </linearGradient>
        </defs>

        <!-- forearm stays put -->
        <rect x="10" y="66" width="90" height="38" rx="18" fill="url(#skin)" stroke="#8b98a9" stroke-width="1.5" />

        <!-- hand pivots at the wrist; palm reaches back under the forearm to hide the joint -->
        <g class="hand">
          <path d="M 78 60 C 104 54 140 54 168 58 C 176 59 178 66 176 72 L 174 88 C 172 96 162 100 148 100 L 88 99 C 79 96 74 84 74 72 Z" fill="url(#skin)" stroke="#8b98a9" stroke-width="1.5" stroke-linejoin="round" />
          <rect x="164" y="55" width="52" height="11" rx="5.5" transform="rotate(-7 164 60.5)" fill="url(#skin)" stroke="#8b98a9" stroke-width="1.5" />
          <rect x="166" y="66" width="50" height="11" rx="5.5" transform="rotate(-2.5 166 71.5)" fill="url(#skin)" stroke="#8b98a9" stroke-width="1.5" />
          <rect x="166" y="77" width="46" height="11" rx="5.5" transform="rotate(2 166 82.5)" fill="url(#skin)" stroke="#8b98a9" stroke-width="1.5" />
          <rect x="164" y="87" width="42" height="10" rx="5" transform="rotate(5.5 164 92)" fill="url(#skin)" stroke="#8b98a9" stroke-width="1.5" />
          <!-- thumb -->
          <path d="M 116 92 C 112 106 120 117 134 117 C 147 117 153 108 151 98 Z" fill="#c3ccd9" stroke="#8b98a9" stroke-width="1.5" stroke-linejoin="round" />
        </g>

        <!-- up = extension, down = flexion -->
        <g class="arrow ext" aria-hidden="true">
          <path d="M 238 50 C 250 34 242 14 222 10" />
          <path class="tip" d="M 222 10 L 236 4 L 236 16 Z" />
        </g>
        <g class="arrow flex" aria-hidden="true">
          <path d="M 238 120 C 250 136 242 156 222 160" />
          <path class="tip" d="M 222 160 L 236 154 L 236 166 Z" />
        </g>
      </svg>
      <div class="wrist-labels">
        <span class="wrist-label ext">Extension</span>
        <span class="wrist-label flex">Flexion</span>
      </div>
    </section>

    <section class="panel">
      <h2>Confidence</h2>
      <svg viewBox="0 0 120 40" role="img" aria-label="Confidence of the last gestures">
        <line v-for="v in [0, 0.5, 1]" :key="v" x1="0" :y1="36 - v * 32" x2="120" :y2="36 - v * 32" class="grid" />
        <polyline v-if="chart.count" :points="chart.line" class="line" />
        <circle v-for="(d, i) in chart.dots" :key="i" :cx="d.cx" :cy="d.cy" r="1.2" :style="{ fill: d.fill }" />
      </svg>
      <div class="axis">
        <span>oldest</span>
        <span>last {{ chart.count }} gestures · confidence 0–100%</span>
        <span>latest</span>
      </div>
    </section>

    <section class="panel">
      <h2>Class distribution</h2>
      <p v-if="!classes.length" class="empty">Waiting for gestures…</p>
      <div v-for="c in classes" :key="c.name" class="row">
        <span class="name">{{ c.name.replace('_', ' ') }}</span>
        <div class="bar" role="progressbar" :aria-valuenow="Math.round(c.percent)" aria-valuemin="0" aria-valuemax="100">
          <div :style="{ width: `${c.percent}%` }" />
        </div>
        <span class="count">{{ c.count }} · {{ Math.round(c.percent) }}%</span>
      </div>
    </section>
  </main>
</template>

<style>
:root {
  color-scheme: dark;
}
* {
  box-sizing: border-box;
}
body {
  margin: 0;
  background: #0f1115;
  color: #e5e7eb;
  font-family: ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif;
}
main {
  max-width: 900px;
  margin: 0 auto;
  padding: 32px 20px 48px;
}
header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 16px;
  margin-bottom: 24px;
}
h1 {
  margin: 0;
  font-size: 28px;
  letter-spacing: -0.02em;
}
header p {
  margin: 4px 0 0;
  color: #9ca3af;
  font-size: 14px;
}
.badge {
  padding: 6px 12px;
  border-radius: 999px;
  font-size: 13px;
  font-weight: 600;
  white-space: nowrap;
}
.badge.on {
  background: #052e1c;
  color: #34d399;
}
.badge.off {
  background: #2b1116;
  color: #f87171;
}
.cards {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
  gap: 12px;
  margin-bottom: 20px;
}
.cards article {
  background: #171a21;
  border: 1px solid #232833;
  border-radius: 12px;
  padding: 16px;
  display: flex;
  flex-direction: column;
  gap: 6px;
  min-height: 96px;
}
.cards span {
  color: #9ca3af;
  font-size: 13px;
}
.cards strong {
  font-size: 22px;
  line-height: 1.2;
}
.cards small {
  color: #9ca3af;
  font-size: 12px;
}
.panel {
  background: #171a21;
  border: 1px solid #232833;
  border-radius: 12px;
  padding: 16px;
  margin-bottom: 20px;
}
.panel h2 {
  margin: 0 0 12px;
  font-size: 16px;
}
svg {
  display: block;
  width: 100%;
  height: auto;
}
.grid {
  stroke: #2a3040;
  stroke-width: 0.3;
}
.line {
  fill: none;
  stroke: #94a3b8;
  stroke-width: 0.7;
  stroke-linejoin: round;
  stroke-linecap: round;
}
.axis {
  display: flex;
  justify-content: space-between;
  color: #6b7280;
  font-size: 12px;
  margin-top: 6px;
}
.wrist {
  max-width: 420px;
  margin: 0 auto;
}
.hand {
  transform-box: view-box;
  transform-origin: 92px 85px;
  animation: wrist 4s ease-in-out infinite;
}
@keyframes wrist {
  0%,
  100% {
    transform: rotate(-24deg);
  }
  50% {
    transform: rotate(24deg);
  }
}
.arrow {
  fill: none;
  stroke-width: 4;
  stroke-linecap: round;
  stroke-linejoin: round;
}
.arrow .tip {
  fill: currentColor;
  stroke: none;
}
.arrow.ext {
  stroke: #34d399;
  color: #34d399;
  animation: fade-ext 4s ease-in-out infinite;
}
.arrow.flex {
  stroke: #60a5fa;
  color: #60a5fa;
  animation: fade-flex 4s ease-in-out infinite;
}
.wrist-labels {
  display: flex;
  justify-content: center;
  gap: 10px;
  margin-top: 12px;
}
.wrist-label {
  padding: 4px 12px;
  border: 1px solid #232833;
  border-radius: 999px;
  color: #6b7280;
  font-size: 12px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.06em;
}
.wrist-label.ext {
  animation: fade-ext 4s ease-in-out infinite;
}
.wrist-label.flex {
  animation: fade-flex 4s ease-in-out infinite;
}
@keyframes fade-ext {
  0%,
  20%,
  100% {
    opacity: 1;
    color: #34d399;
  }
  40%,
  80% {
    opacity: 0.25;
    color: #6b7280;
  }
}
@keyframes fade-flex {
  0%,
  30%,
  100% {
    opacity: 0.25;
    color: #6b7280;
  }
  50%,
  70% {
    opacity: 1;
    color: #60a5fa;
  }
}
@media (prefers-reduced-motion: reduce) {
  .hand {
    animation: none;
  }
  .arrow.ext,
  .arrow.flex,
  .wrist-label.ext,
  .wrist-label.flex {
    animation: none;
  }
  .arrow.ext {
    stroke: #34d399;
  }
  .arrow.flex {
    stroke: #60a5fa;
  }
}
.empty {
  color: #6b7280;
  font-size: 14px;
  margin: 0;
}
.row {
  display: grid;
  grid-template-columns: 130px 1fr 80px;
  align-items: center;
  gap: 12px;
  padding: 6px 0;
}
.row .name {
  font-size: 13px;
  color: #d1d5db;
}
.row .count {
  font-size: 13px;
  color: #9ca3af;
  text-align: right;
}
.bar {
  height: 8px;
  background: #232833;
  border-radius: 999px;
  overflow: hidden;
}
.bar div {
  height: 100%;
  background: linear-gradient(90deg, #34d399, #60a5fa);
  border-radius: 999px;
  transition: width 0.3s ease;
}
@media (max-width: 480px) {
  .row {
    grid-template-columns: 100px 1fr 70px;
  }
}
</style>
