#include <pebble.h>

// Tunables
#define FRAME_MS             33
#define BRICK_ROWS           4
#define BRICK_COLS           6
#define BRICK_HEIGHT         10
#define BALL_SIZE            5
#define PADDLE_H             5
#define SCORE_BAR_H          14
#define STARTING_LIVES       3
#define BALL_VX_MAX          4

#if defined(PBL_ROUND)
  #define PLAY_INSET_X       30
  #define HUD_TOP_OFFSET     14
  #define PLAYFIELD_EXTRA    18
#else
  #define PLAY_INSET_X       0
  #define HUD_TOP_OFFSET     0
  #define PLAYFIELD_EXTRA    0
#endif

typedef enum {
  StateWaiting,
  StatePlaying,
  StateGameOver,
  StateWin,
} GameState;

#if defined(PBL_SPEAKER)
#define BOOP_VOLUME 80
#define BOOP_NOTE(midi, wave, dur) \
  { .midi_note = (midi), .waveform = (wave), .duration_ms = (dur), .velocity = 0, .reserved = 0 }

// Paddle bounce — short pop
static const SpeakerNote s_boop_paddle[] = {
  BOOP_NOTE(72, SpeakerWaveformSquare, 130), // C5
};

// Wall bounce — low tick
static const SpeakerNote s_boop_wall[] = {
  BOOP_NOTE(64, SpeakerWaveformSquare, 110), // E4
};

// Brick hit — pitch ascends with row position (top row = highest)
static const SpeakerNote s_boop_brick_r0[] = { BOOP_NOTE(79, SpeakerWaveformSquare, 130) }; // G5
static const SpeakerNote s_boop_brick_r1[] = { BOOP_NOTE(77, SpeakerWaveformSquare, 130) }; // F5
static const SpeakerNote s_boop_brick_r2[] = { BOOP_NOTE(76, SpeakerWaveformSquare, 130) }; // E5
static const SpeakerNote s_boop_brick_r3[] = { BOOP_NOTE(74, SpeakerWaveformSquare, 130) }; // D5
static const SpeakerNote *const s_boop_brick_by_row[BRICK_ROWS] = {
  s_boop_brick_r0, s_boop_brick_r1, s_boop_brick_r2, s_boop_brick_r3,
};

// Life lost — descending blip
static const SpeakerNote s_boop_life[] = {
  BOOP_NOTE(67, SpeakerWaveformTriangle, 90),  // G4
  BOOP_NOTE(60, SpeakerWaveformTriangle, 130), // C4
};

// Game over — sad descend
static const SpeakerNote s_boop_gameover[] = {
  BOOP_NOTE(60, SpeakerWaveformTriangle, 160), // C4
  BOOP_NOTE(57, SpeakerWaveformTriangle, 160), // A3
  BOOP_NOTE(53, SpeakerWaveformTriangle, 280), // F3
};

// Win — little fanfare
static const SpeakerNote s_boop_win[] = {
  BOOP_NOTE(72, SpeakerWaveformSquare, 90),  // C5
  BOOP_NOTE(76, SpeakerWaveformSquare, 90),  // E5
  BOOP_NOTE(79, SpeakerWaveformSquare, 90),  // G5
  BOOP_NOTE(84, SpeakerWaveformSquare, 220), // C6
};

static time_t s_boop_last_s;
static uint16_t s_boop_last_ms;

static void play_boop(const SpeakerNote *notes, uint32_t count) {
  // Cooldown so rapid collisions don't preempt mid-note and silence each other.
  time_t now_s;
  uint16_t now_ms;
  time_ms(&now_s, &now_ms);
  int32_t elapsed = (int32_t)(now_s - s_boop_last_s) * 1000 +
                    (int32_t)now_ms - (int32_t)s_boop_last_ms;
  if (elapsed >= 0 && elapsed < 70) return;
  s_boop_last_s = now_s;
  s_boop_last_ms = now_ms;
  speaker_play_notes(notes, count, BOOP_VOLUME);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "boop count=%lu", (unsigned long)count);
}
#define PLAY_BOOP(arr) play_boop((arr), ARRAY_LENGTH(arr))
#else
#define PLAY_BOOP(arr) ((void)0)
#endif

static Window *s_window;
static Layer  *s_canvas;
static AppTimer *s_timer;

static int16_t s_screen_w;
static int16_t s_screen_h;
static int16_t s_brick_w;
static int16_t s_playfield_top;
static int16_t s_paddle_y;
static int16_t s_paddle_w;
static int16_t s_play_left;
static int16_t s_play_right;

static int16_t s_paddle_x;
static int16_t s_ball_x, s_ball_y;
static int16_t s_ball_vx, s_ball_vy;

static bool s_bricks[BRICK_ROWS][BRICK_COLS];
static int  s_bricks_left;
static int  s_score;
static int  s_lives;
static GameState s_state;

// Game logic

static void reset_bricks(void) {
  s_bricks_left = BRICK_ROWS * BRICK_COLS;
  for (int r = 0; r < BRICK_ROWS; r++) {
    for (int c = 0; c < BRICK_COLS; c++) {
      s_bricks[r][c] = true;
    }
  }
}

static void park_ball_on_paddle(void) {
  s_ball_x = s_paddle_x + s_paddle_w / 2 - BALL_SIZE / 2;
  s_ball_y = s_paddle_y - BALL_SIZE - 1;
  s_ball_vx = 2;
  s_ball_vy = -3;
}

static void start_new_game(void) {
  s_score = 0;
  s_lives = STARTING_LIVES;
  s_paddle_x = (s_play_left + s_play_right - s_paddle_w) / 2;
  reset_bricks();
  park_ball_on_paddle();
  s_state = StateWaiting;
}

static void launch_or_restart(void) {
  if (s_state == StateWaiting) {
    s_state = StatePlaying;
  } else if (s_state == StateGameOver || s_state == StateWin) {
    start_new_game();
  }
}

static void clamp_paddle(void) {
  if (s_paddle_x < s_play_left) s_paddle_x = s_play_left;
  if (s_paddle_x + s_paddle_w > s_play_right) s_paddle_x = s_play_right - s_paddle_w;
}

static void update_ball(void) {
  int16_t nx = s_ball_x + s_ball_vx;
  int16_t ny = s_ball_y + s_ball_vy;

  if (nx < s_play_left) {
    nx = s_play_left;
    s_ball_vx = -s_ball_vx;
    PLAY_BOOP(s_boop_wall);
  } else if (nx + BALL_SIZE > s_play_right) {
    nx = s_play_right - BALL_SIZE;
    s_ball_vx = -s_ball_vx;
    PLAY_BOOP(s_boop_wall);
  }
  if (ny < s_playfield_top) {
    ny = s_playfield_top;
    s_ball_vy = -s_ball_vy;
    PLAY_BOOP(s_boop_wall);
  }

  // Paddle
  if (s_ball_vy > 0 &&
      ny + BALL_SIZE >= s_paddle_y &&
      ny <= s_paddle_y + PADDLE_H &&
      nx + BALL_SIZE > s_paddle_x &&
      nx < s_paddle_x + s_paddle_w) {
    ny = s_paddle_y - BALL_SIZE;
    s_ball_vy = -s_ball_vy;
    int16_t ball_center = nx + BALL_SIZE / 2;
    int16_t paddle_center = s_paddle_x + s_paddle_w / 2;
    int16_t offset = ball_center - paddle_center;
    int16_t half = s_paddle_w / 2;
    int16_t new_vx = half > 0 ? (offset * 3) / half : 0;
    if (new_vx == 0) new_vx = (s_ball_vx >= 0) ? 1 : -1;
    if (new_vx > BALL_VX_MAX) new_vx = BALL_VX_MAX;
    if (new_vx < -BALL_VX_MAX) new_vx = -BALL_VX_MAX;
    s_ball_vx = new_vx;
    PLAY_BOOP(s_boop_paddle);
  }

  // Bricks (one hit per frame)
  bool hit = false;
  for (int r = 0; r < BRICK_ROWS && !hit; r++) {
    for (int c = 0; c < BRICK_COLS && !hit; c++) {
      if (!s_bricks[r][c]) continue;
      int16_t bx = s_play_left + c * s_brick_w;
      int16_t by = s_playfield_top + r * BRICK_HEIGHT;
      if (nx + BALL_SIZE > bx && nx < bx + s_brick_w &&
          ny + BALL_SIZE > by && ny < by + BRICK_HEIGHT) {
        s_bricks[r][c] = false;
        s_bricks_left--;
        s_score += 10;
        bool horizontal = (s_ball_x + BALL_SIZE <= bx) ||
                          (s_ball_x >= bx + s_brick_w);
        if (horizontal) s_ball_vx = -s_ball_vx;
        else            s_ball_vy = -s_ball_vy;
        hit = true;
#if defined(PBL_SPEAKER)
        play_boop(s_boop_brick_by_row[r], 1);
#endif
      }
    }
  }

  // Dead zone
  if (ny > s_screen_h) {
    s_lives--;
    if (s_lives <= 0) {
      s_state = StateGameOver;
      PLAY_BOOP(s_boop_gameover);
    } else {
      park_ball_on_paddle();
      s_state = StateWaiting;
      PLAY_BOOP(s_boop_life);
    }
    return;
  }

  s_ball_x = nx;
  s_ball_y = ny;

  if (s_bricks_left == 0) {
    s_state = StateWin;
    PLAY_BOOP(s_boop_win);
  }
}

// Rendering

static GColor brick_color(int row) {
#if defined(PBL_COLOR)
  switch (row) {
    case 0: return GColorRed;
    case 1: return GColorOrange;
    case 2: return GColorChromeYellow;
    case 3: return GColorIslamicGreen;
    default: return GColorCyan;
  }
#else
  (void)row;
  return GColorWhite;
#endif
}

static void draw_centered(GContext *ctx, const char *text, int y, GFont font, int width) {
  graphics_draw_text(ctx, text, font,
                     GRect(0, y, width, 36),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // HUD
  char buf[32];
#if defined(PBL_ROUND)
  snprintf(buf, sizeof(buf), "%d  L%d", s_score, s_lives);
#else
  snprintf(buf, sizeof(buf), "SCORE %d   LIVES %d", s_score, s_lives);
#endif
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(0, HUD_TOP_OFFSET - 2, bounds.size.w, SCORE_BAR_H + 4),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // Bricks
  for (int r = 0; r < BRICK_ROWS; r++) {
    for (int c = 0; c < BRICK_COLS; c++) {
      if (!s_bricks[r][c]) continue;
      int16_t bx = s_play_left + c * s_brick_w;
      int16_t by = s_playfield_top + r * BRICK_HEIGHT;
      graphics_context_set_fill_color(ctx, brick_color(r));
      graphics_fill_rect(ctx, GRect(bx + 1, by + 1, s_brick_w - 2, BRICK_HEIGHT - 2),
                         0, GCornerNone);
    }
  }

  // Paddle
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(s_paddle_x, s_paddle_y, s_paddle_w, PADDLE_H),
                     2, GCornersAll);

  // Ball
  graphics_fill_circle(ctx,
                       GPoint(s_ball_x + BALL_SIZE / 2, s_ball_y + BALL_SIZE / 2),
                       BALL_SIZE / 2 + 1);

  // Overlay messages
  GFont big   = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GFont small = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  int mid_y = bounds.size.h / 2 - 24;
  graphics_context_set_text_color(ctx, GColorWhite);

  switch (s_state) {
    case StateWaiting:
      draw_centered(ctx, "TAP TO PLAY", mid_y, big, bounds.size.w);
      break;
    case StateGameOver:
      draw_centered(ctx, "GAME OVER", mid_y, big, bounds.size.w);
      draw_centered(ctx, "Tap to retry", mid_y + 30, small, bounds.size.w);
      break;
    case StateWin:
      draw_centered(ctx, "YOU WIN!", mid_y, big, bounds.size.w);
      draw_centered(ctx, "Tap to play again", mid_y + 30, small, bounds.size.w);
      break;
    case StatePlaying:
      break;
  }
}

// Input

#if defined(PBL_TOUCH)
static void touch_handler(const TouchEvent *event, void *context) {
  switch (event->type) {
    case TouchEvent_Touchdown:
      s_paddle_x = event->x - s_paddle_w / 2;
      clamp_paddle();
      launch_or_restart();
      break;
    case TouchEvent_PositionUpdate:
      s_paddle_x = event->x - s_paddle_w / 2;
      clamp_paddle();
      break;
    case TouchEvent_Liftoff:
      break;
  }
}
#endif

// Timer

static void timer_callback(void *ctx) {
  s_timer = NULL;
  if (s_state == StatePlaying) {
    update_ball();
  } else if (s_state == StateWaiting) {
    // Ball rides the paddle while waiting for launch
    s_ball_x = s_paddle_x + s_paddle_w / 2 - BALL_SIZE / 2;
    s_ball_y = s_paddle_y - BALL_SIZE - 1;
  }
  layer_mark_dirty(s_canvas);
  s_timer = app_timer_register(FRAME_MS, timer_callback, NULL);
}

// Window lifecycle

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_screen_w = bounds.size.w;
  s_screen_h = bounds.size.h;
  s_play_left = PLAY_INSET_X;
  s_play_right = s_screen_w - PLAY_INSET_X;
  s_brick_w = (s_play_right - s_play_left) / BRICK_COLS;
  s_playfield_top = SCORE_BAR_H + 4 + HUD_TOP_OFFSET + PLAYFIELD_EXTRA;
  s_paddle_w = (s_play_right - s_play_left) / 4;
  if (s_paddle_w < 24) s_paddle_w = 24;
  s_paddle_y = s_screen_h - 48;

  window_set_background_color(window, GColorBlack);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);

  start_new_game();
}

static void window_appear(Window *window) {
#if defined(PBL_TOUCH)
  if (touch_service_is_enabled()) {
    touch_service_subscribe(touch_handler, NULL);
  }
#endif
  s_timer = app_timer_register(FRAME_MS, timer_callback, NULL);
}

static void window_disappear(Window *window) {
#if defined(PBL_TOUCH)
  touch_service_unsubscribe();
#endif
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
#if defined(PBL_SPEAKER)
  speaker_stop();
#endif
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
}

static void prv_init(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load       = window_load,
    .appear     = window_appear,
    .disappear  = window_disappear,
    .unload     = window_unload,
  });
  window_stack_push(s_window, true);
}

static void prv_deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
