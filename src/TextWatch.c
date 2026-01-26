#include <pebble.h>
#include <time.h>

#include "num2words.h"
#include "AppRequests.h"

#define NUM_LINES 4
#define LINE_LENGTH 7
#define BUFFER_SIZE (LINE_LENGTH + 2)
#define ROW_HEIGHT 37
#define TEXT_LAYER_HEIGHT 50
#define SCREEN_HEIGHT 168
#define TOP_TEXT_RESERVE 21
#define BOTTOM_TEXT_RESERVE 21
#define BOTTOM_ARROW_WIDTH 18
#define DATE_BUFFER_SIZE 16
#define INFO_BUFFER_SIZE 24

// Message keys must match package.json messageKeys
#define INVERT_KEY 0
#define TEXT_ALIGN_KEY 1
#define LANGUAGE_KEY 2

#define TEXT_ALIGN_CENTER 0
#define TEXT_ALIGN_LEFT 1
#define TEXT_ALIGN_RIGHT 2

// The time it takes for a layer to slide in or out.
#define ANIMATION_DURATION 400
// Delay between the layers animations, from top to bottom
#define ANIMATION_STAGGER_TIME 150
// Delay from the start of the current layer going out until the next layer slides in
#define ANIMATION_OUT_IN_DELAY 100

#define LINE_APPEND_MARGIN 0
// We can add a new word to a line if there are at least this many characters free after
#define LINE_APPEND_LIMIT (LINE_LENGTH - LINE_APPEND_MARGIN)

static AppSync sync;
static uint8_t sync_buffer[64];

static int text_align = TEXT_ALIGN_CENTER;
static bool invert = false;
static Language lang = EN_US;

static Window *window;

typedef struct {
	TextLayer *currentLayer;
	TextLayer *nextLayer;
	char lineStr1[BUFFER_SIZE];
	char lineStr2[BUFFER_SIZE];
	PropertyAnimation *animation1;
	PropertyAnimation *animation2;
} Line;

static Line lines[NUM_LINES];
static Layer *inverter_layer;
static Layer *top_info_layer;
static Layer *bottom_info_background_layer;
static TextLayer *bottom_date_layer;
static TextLayer *bottom_info_layer;
static Layer *bottom_arrow_layer;
static BatteryChargeState current_battery_state;
static bool bluetooth_connected;
static char top_time_buffer[6];
static char bottom_date_buffer[DATE_BUFFER_SIZE];
static char bottom_info_buffer[INFO_BUFFER_SIZE];
static int bottom_trend_direction = TREND_UNKNOWN;

static void top_info_update_proc(Layer *layer, GContext *ctx);
static void battery_state_handler(BatteryChargeState state);
static void bluetooth_handler(bool connected);
static void update_top_time_buffer(struct tm *time);
static void update_bottom_status(struct tm *time);
static void apply_bottom_theme(void);
static void bottom_arrow_update_proc(Layer *layer, GContext *ctx);
static void bottom_info_background_update_proc(Layer *layer, GContext *ctx);

static void inverter_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  // Zeichnet ein einfaches Highlight. Passe GColor an dein Design an:
  // z.B. GColorWhite für heller Hintergrund, GColorBlack für dunkles Design.
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
}


static void update_top_time_buffer(struct tm *time) {
	if (!time) {
		return;
	}
	const char *format = clock_is_24h_style() ? "%H:%M" : "%I:%M";
	if (strftime(top_time_buffer, sizeof(top_time_buffer), format, time) == 0) {
		strncpy(top_time_buffer, "--:--", sizeof(top_time_buffer));
		top_time_buffer[sizeof(top_time_buffer) - 1] = '\0';
	}
	if (!clock_is_24h_style() && top_time_buffer[0] == '0') {
		memmove(top_time_buffer, top_time_buffer + 1, sizeof(top_time_buffer) - 1);
	}
	if (top_info_layer) {
		layer_mark_dirty(top_info_layer);
	}
}

static void draw_bluetooth_icon(GContext *ctx, GColor color, GRect bounds, int center_y) {
	const int origin_x = bounds.origin.x + 10;
	const int half_height = 6;
	const int wing_dx = 5;
	const int wing_dy = 3;
	graphics_context_set_stroke_color(ctx, color);
	graphics_draw_line(ctx, GPoint(origin_x, center_y - half_height), GPoint(origin_x, center_y + half_height));
	graphics_draw_line(ctx, GPoint(origin_x, center_y - half_height), GPoint(origin_x + wing_dx, center_y - wing_dy));
	graphics_draw_line(ctx, GPoint(origin_x, center_y), GPoint(origin_x + wing_dx, center_y - wing_dy));
	graphics_draw_line(ctx, GPoint(origin_x, center_y), GPoint(origin_x - wing_dx, center_y - wing_dy));
	graphics_draw_line(ctx, GPoint(origin_x, center_y), GPoint(origin_x + wing_dx, center_y + wing_dy));
	graphics_draw_line(ctx, GPoint(origin_x, center_y), GPoint(origin_x - wing_dx, center_y + wing_dy));
	graphics_draw_line(ctx, GPoint(origin_x, center_y + half_height), GPoint(origin_x + wing_dx, center_y + wing_dy));
	if (!bluetooth_connected) {
		// Overlay a thicker diagonal strike to signal the disconnected state
		const GPoint strike_start = GPoint(origin_x - wing_dx - 1, center_y - half_height - 1);
		const GPoint strike_end = GPoint(origin_x + wing_dx + 2, center_y + half_height + 2);
		graphics_draw_line(ctx, strike_start, strike_end);
		graphics_draw_line(ctx,
			GPoint(strike_start.x + 1, strike_start.y),
			GPoint(strike_end.x + 1, strike_end.y));
	}
}

static void draw_battery_icon(GContext *ctx, GColor color, GRect bounds, int center_y) {
	const int battery_height = 9;
	const int battery_width = 16;
	const int margin = 6;
	const int y = center_y - battery_height / 2;
	const int x = bounds.origin.x + bounds.size.w - battery_width - margin;
	GRect body = GRect(x, y, battery_width, battery_height);
	GRect cap = GRect(x + battery_width, y + 2, 2, battery_height - 4);
	graphics_context_set_stroke_color(ctx, color);
	graphics_draw_rect(ctx, body);
	graphics_context_set_fill_color(ctx, color);
	graphics_fill_rect(ctx, cap, 0, GCornerNone);
	int inner_width = body.size.w - 2;
	if (inner_width < 0) {
		inner_width = 0;
	}
	int fill_width = (inner_width * current_battery_state.charge_percent) / 100;
	if (fill_width > inner_width) {
		fill_width = inner_width;
	}
	if (fill_width < 0) {
		fill_width = 0;
	}
	if (current_battery_state.charge_percent > 0 || current_battery_state.is_charging) {
		if (fill_width == 0 && current_battery_state.charge_percent > 0 && inner_width > 0) {
			fill_width = 1;
		}
		if (fill_width > 0) {
			graphics_fill_rect(ctx, GRect(body.origin.x + 1, body.origin.y + 1, fill_width, body.size.h - 2), 0, GCornerNone);
		}
	}
	if (current_battery_state.is_charging) {
		bool base_is_white = gcolor_equal(color, GColorWhite);
		GColor bolt_color = base_is_white ? GColorBlack : GColorWhite;
		graphics_context_set_stroke_color(ctx, bolt_color);
		int bolt_x = body.origin.x + body.size.w / 2 - 2;
		int bolt_y = body.origin.y + 1;
		graphics_draw_line(ctx, GPoint(bolt_x, bolt_y), GPoint(bolt_x + 3, bolt_y + 4));
		graphics_draw_line(ctx, GPoint(bolt_x + 3, bolt_y + 4), GPoint(bolt_x + 1, bolt_y + 4));
		graphics_draw_line(ctx, GPoint(bolt_x + 1, bolt_y + 4), GPoint(bolt_x + 4, bolt_y + 8));
	}
	graphics_context_set_stroke_color(ctx, color);
}

static void top_info_update_proc(Layer *layer, GContext *ctx) {
	GRect bounds = layer_get_bounds(layer);
	GColor fg = invert ? GColorBlack : GColorWhite;
	GColor bg = invert ? GColorWhite : GColorBlack;
	const int center_y = bounds.origin.y + bounds.size.h / 2;
	graphics_context_set_fill_color(ctx, bg);
	graphics_fill_rect(ctx, bounds, 0, GCornerNone);
	graphics_context_set_text_color(ctx, fg);
	GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
	const char *time_text = strlen(top_time_buffer) > 0 ? top_time_buffer : "--:--";
	GRect measure_rect = GRect(0, 0, bounds.size.w - 60, bounds.size.h);
	GSize text_size = graphics_text_layout_get_content_size(time_text, font, measure_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter);
	if (text_size.h <= 0) {
		text_size.h = 17;
	}
	int text_y = center_y - text_size.h / 2 - 3;

	GRect time_bounds = GRect(bounds.origin.x + 30, text_y, bounds.size.w - 60, text_size.h);
	graphics_draw_text(ctx,
		time_text,
		font,
		time_bounds,
		GTextOverflowModeTrailingEllipsis,
		GTextAlignmentCenter,
		NULL);
	graphics_context_set_stroke_color(ctx, fg);
	graphics_context_set_fill_color(ctx, fg);
	graphics_draw_line(ctx,
		GPoint(bounds.origin.x, center_y + 10),
		GPoint(bounds.origin.x + bounds.size.w, center_y + 10));
	draw_bluetooth_icon(ctx, fg, bounds, center_y);
	draw_battery_icon(ctx, fg, bounds, center_y);
}

static void battery_state_handler(BatteryChargeState state) {
	current_battery_state = state;
	if (top_info_layer) {
		layer_mark_dirty(top_info_layer);
	}
}

static void bluetooth_handler(bool connected) {
	bluetooth_connected = connected;
	if (top_info_layer) {
		layer_mark_dirty(top_info_layer);
	}
}

static void apply_bottom_theme(void) {
	GColor text_color = invert ? GColorBlack : GColorWhite;
	if (bottom_date_layer) {
		text_layer_set_text_color(bottom_date_layer, text_color);
	}
	if (bottom_info_layer) {
		text_layer_set_text_color(bottom_info_layer, text_color);
	}
	if (bottom_arrow_layer) {
		layer_mark_dirty(bottom_arrow_layer);
	}
	if (bottom_info_background_layer) {
		layer_mark_dirty(bottom_info_background_layer);
	}
}

static void bottom_info_background_update_proc(Layer *layer, GContext *ctx) {
	GRect bounds = layer_get_bounds(layer);
	GColor bg = invert ? GColorWhite : GColorBlack;
	GColor fg = invert ? GColorBlack : GColorWhite;
	graphics_context_set_fill_color(ctx, bg);
	graphics_fill_rect(ctx, bounds, 0, GCornerNone);
	graphics_context_set_stroke_color(ctx, fg);
	const int center_y = bounds.origin.y + bounds.size.h / 2;
	graphics_draw_line(ctx, GPoint(bounds.origin.x, center_y - 10), GPoint(bounds.origin.x + bounds.size.w, center_y - 10));
}

static void get_glucose_data(int *glucose_value, int *trend_value) {
	// Get the last received values from the messenger
	pebble_messenger_get_glucose(glucose_value, trend_value);
}

// Callback when new glucose data is received from phone
static void glucose_data_received_callback(int glucose_value, int trend_value) {
	APP_LOG(APP_LOG_LEVEL_INFO, "Glucose data received: %d mg/dL, trend: %d", glucose_value, trend_value);
	
	// Update the trend direction for the arrow
	bottom_trend_direction = trend_value;
	
	// Update the glucose display
	if (glucose_value > 0) {
		snprintf(bottom_info_buffer, sizeof(bottom_info_buffer), "%d", glucose_value);
	} else {
		snprintf(bottom_info_buffer, sizeof(bottom_info_buffer), "---");
	}
	
	// Refresh the display layers
	if (bottom_info_layer) {
		text_layer_set_text(bottom_info_layer, bottom_info_buffer);
	}
	if (bottom_arrow_layer) {
		layer_mark_dirty(bottom_arrow_layer);
	}
}

static void draw_arrow_shape(GContext *ctx, GPoint center, GPoint tip, GColor color) {
	graphics_context_set_stroke_color(ctx, color);
	graphics_draw_line(ctx, center, tip);

	const int head_size = 4;
	int dx = tip.x - center.x;
	int dy = tip.y - center.y;

	GPoint left;
	GPoint right;
	if (dx == 0 && dy < 0) { // Up
		left = GPoint(tip.x - head_size, tip.y + head_size);
		right = GPoint(tip.x + head_size, tip.y + head_size);
	} else if (dx > 0 && dy < 0) { // Up-right
		left = GPoint(tip.x - head_size, tip.y + head_size / 2);
		right = GPoint(tip.x - head_size / 2, tip.y + head_size);
	} else if (dx > 0 && dy == 0) { // Right
		left = GPoint(tip.x - head_size, tip.y - head_size);
		right = GPoint(tip.x - head_size, tip.y + head_size);
	} else if (dx > 0 && dy > 0) { // Down-right
		left = GPoint(tip.x - head_size, tip.y - head_size / 2);
		right = GPoint(tip.x - head_size / 2, tip.y - head_size);
	} else if (dx == 0 && dy > 0) { // Down
		left = GPoint(tip.x - head_size, tip.y - head_size);
		right = GPoint(tip.x + head_size, tip.y - head_size);
	} else if (dx < 0 && dy > 0) { // Down-left
		left = GPoint(tip.x + head_size, tip.y - head_size / 2);
		right = GPoint(tip.x + head_size / 2, tip.y - head_size);
	} else if (dx < 0 && dy == 0) { // Left
		left = GPoint(tip.x + head_size, tip.y - head_size);
		right = GPoint(tip.x + head_size, tip.y + head_size);
	} else { // Up-left or fallback
		left = GPoint(tip.x + head_size, tip.y + head_size / 2);
		right = GPoint(tip.x + head_size / 2, tip.y + head_size);
	}

	graphics_draw_line(ctx, tip, left);
	graphics_draw_line(ctx, tip, right);
}

static void bottom_arrow_update_proc(Layer *layer, GContext *ctx) {
	GRect bounds = layer_get_bounds(layer);
	GPoint center = GPoint(bounds.size.w / 2, bounds.size.h / 2);
	const int length = (bounds.size.w < bounds.size.h ? bounds.size.w : bounds.size.h) / 2 - 2;
	GColor color = invert ? GColorBlack : GColorWhite;
	graphics_context_set_stroke_color(ctx, color);

	GPoint tip = center;
	switch (bottom_trend_direction) {
		case TREND_DOUBLE_UP:
			tip.y -= length;
			break;
		case TREND_SINGLE_UP:
			tip.x += length;
			tip.y -= length;
			break;
		case TREND_FORTY_FIVE_UP:
			tip.x += length;
			break;
		case TREND_FLAT:
			tip.x += length;
			break;
		case TREND_FORTY_FIVE_DOWN:
			tip.y += length;
			break;
		case TREND_SINGLE_DOWN:
			tip.x -= length;
			break;
		case TREND_DOUBLE_DOWN:
			tip.y += length;
			break;
		default:
			return;
	}

	draw_arrow_shape(ctx, center, tip, color);
}

static void update_bottom_status(struct tm *time) {
	if (!time) {
		return;
	}
	// Update date display
	if (strftime(bottom_date_buffer, sizeof(bottom_date_buffer), "%d.%m.%Y", time) == 0) {
		strncpy(bottom_date_buffer, "--.--.----", sizeof(bottom_date_buffer));
		bottom_date_buffer[sizeof(bottom_date_buffer) - 1] = '\0';
	}
	
	// Get glucose data from messenger (don't request here - requests happen every 5 min in tick handler)
	int glucose_value = 0;
	int trend_value = TREND_UNKNOWN;
	get_glucose_data(&glucose_value, &trend_value);
	
	// Update trend direction for arrow display
	bottom_trend_direction = trend_value;
	
	// Format glucose display - show "---" if no data
	if (glucose_value > 0) {
		snprintf(bottom_info_buffer, sizeof(bottom_info_buffer), "%d", glucose_value);
	} else {
		snprintf(bottom_info_buffer, sizeof(bottom_info_buffer), "---");
	}
	
	// Update display layers
	if (bottom_date_layer) {
		text_layer_set_text(bottom_date_layer, bottom_date_buffer);
	}
	if (bottom_info_layer) {
		text_layer_set_text(bottom_info_layer, bottom_info_buffer);
	}
	if (bottom_arrow_layer) {
		layer_mark_dirty(bottom_arrow_layer);
	}
}

static struct tm current_time;  // Store actual time data, not just a pointer
static struct tm *t = &current_time;  // Keep pointer for compatibility

static int currentNLines;

static bool showTime = true;
static int dateTimeout = 0;

// Animation handler
static void animationStoppedHandler(struct Animation *animation, bool finished, void *context)
{
	TextLayer *current = (TextLayer *)context;
	GRect rect = layer_get_frame((Layer *)current);
	rect.origin.x = 144;
	layer_set_frame((Layer *)current, rect);
}

// // Animate line
// static void makeAnimationsForLayer(Line *line, int delay)
// {
// 	TextLayer *current = line->currentLayer;
// 	TextLayer *next = line->nextLayer;

// 	// Destroy old animations
// 	if (line->animation1 != NULL)
// 	{
// 		line->animation1 = NULL;
// 	}
// 	if (line->animation2 != NULL)
// 	{
// 		line->animation2 = NULL;
// 	}

// 	// Configure animation for current layer to move out
// 	GRect rect = layer_get_frame((Layer *)current);
// 	rect.origin.x =  -144;
// 	line->animation1 = property_animation_create_layer_frame((Layer *)current, NULL, &rect);
// 	animation_set_duration(&line->animation1->animation, ANIMATION_DURATION);
// 	animation_set_delay(&line->animation1->animation, delay);
// 	animation_set_curve(&line->animation1->animation, AnimationCurveEaseIn); // Accelerate

// 	// Configure animation for current layer to move in
// 	GRect rect2 = layer_get_frame((Layer *)next);
// 	rect2.origin.x = 0;
// 	line->animation2 = property_animation_create_layer_frame((Layer *)next, NULL, &rect2);
// 	animation_set_duration(&line->animation2->animation, ANIMATION_DURATION);
// 	animation_set_delay(&line->animation2->animation, delay + ANIMATION_OUT_IN_DELAY);
// 	animation_set_curve(&line->animation2->animation, AnimationCurveEaseOut); // Deaccelerate

// 	// Set a handler to rearrange layers after animation is finished
// 	animation_set_handlers(&line->animation2->animation, (AnimationHandlers) {
// 		.stopped = (AnimationStoppedHandler)animationStoppedHandler
// 	}, current);

// 	// Start the animations
// 	animation_schedule(&line->animation1->animation);
// 	animation_schedule(&line->animation2->animation);	
// }
static void makeAnimationsForLayer(Line *line, int delay) {
    if (!line) return;

    TextLayer *current = line->currentLayer;
    TextLayer *next = line->nextLayer;

    // Clear old animation pointers (animations auto-destroy when finished)
    // We don't manually destroy them since they may already be destroyed
    line->animation1 = NULL;
    line->animation2 = NULL;

    // --- Create first property animation (move current out) ---
    GRect rect_current = layer_get_frame((Layer *)current);
    rect_current.origin.x = -144;
    line->animation1 = property_animation_create_layer_frame((Layer *)current, NULL, &rect_current);
    
    if (line->animation1) {
        Animation *anim1 = property_animation_get_animation(line->animation1);
        if (anim1) {
            animation_set_duration(anim1, ANIMATION_DURATION);
            animation_set_delay(anim1, delay);
            animation_set_curve(anim1, AnimationCurveEaseIn);
            animation_schedule(anim1);
        }
    }

    // --- Create second property animation (move next in) ---
    GRect rect_next = layer_get_frame((Layer *)next);
    rect_next.origin.x = 0;
    line->animation2 = property_animation_create_layer_frame((Layer *)next, NULL, &rect_next);
    
    if (line->animation2) {
        Animation *anim2 = property_animation_get_animation(line->animation2);
        if (anim2) {
            animation_set_duration(anim2, ANIMATION_DURATION);
            animation_set_delay(anim2, delay + ANIMATION_OUT_IN_DELAY);
            animation_set_curve(anim2, AnimationCurveEaseOut);
            
            AnimationHandlers handlers = {
                .stopped = (AnimationStoppedHandler)animationStoppedHandler
            };
            animation_set_handlers(anim2, handlers, current);
            animation_schedule(anim2);
        }
    }
}




static void updateLayerText(Line *line, TextLayer *layer, const char *text)
{
	if (!line || !layer || !text) {
		return;
	}

	char *target_buffer = (layer == line->currentLayer) ? line->lineStr1 : line->lineStr2;
	strncpy(target_buffer, text, BUFFER_SIZE - 1);
	target_buffer[BUFFER_SIZE - 1] = '\0';

	text_layer_set_text(layer, target_buffer);
}

// Update line
static void updateLineTo(Line *line, char *value, int delay)
{
	updateLayerText(line, line->nextLayer, value);
	makeAnimationsForLayer(line, delay);

	// Swap current/next layers
	TextLayer *tmp = line->nextLayer;
	line->nextLayer = line->currentLayer;
	line->currentLayer = tmp;
}

// Check to see if the current line needs to be updated
static bool needToUpdateLine(Line *line, char *nextValue)
{
	const char *currentStr = text_layer_get_text(line->currentLayer);

	if (strcmp(currentStr, nextValue) != 0) {
		return true;
	}
	return false;
}

static GTextAlignment lookup_text_alignment(int align_key)
{
	GTextAlignment alignment;
	switch (align_key)
	{
		case TEXT_ALIGN_LEFT:
			alignment = GTextAlignmentLeft;
			break;
		case TEXT_ALIGN_RIGHT:
			alignment = GTextAlignmentRight;
			break;
		default:
			alignment = GTextAlignmentCenter;
			break;
	}
	return alignment;
}

// Configure bold line of text
static void configureBoldLayer(TextLayer *textlayer)
{
    text_layer_set_font(textlayer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
    text_layer_set_text_color(textlayer, invert ? GColorBlack : GColorWhite);
    text_layer_set_background_color(textlayer, GColorClear);
    text_layer_set_text_alignment(textlayer, lookup_text_alignment(text_align));
}

// Configure light line of text
static void configureLightLayer(TextLayer *textlayer)
{
    text_layer_set_font(textlayer, fonts_get_system_font(FONT_KEY_BITHAM_42_LIGHT));
    text_layer_set_text_color(textlayer, invert ? GColorBlack : GColorWhite);
    text_layer_set_background_color(textlayer, GColorClear);
    text_layer_set_text_alignment(textlayer, lookup_text_alignment(text_align));
}

// Configure the layers for the given text
static int configureLayersForText(char text[NUM_LINES][BUFFER_SIZE], char format[])
{
	int numLines = 0;

	// Set bold layer.
	int i;
	for (i = 0; i < NUM_LINES; i++) {
		if (strlen(text[i]) > 0) {
			if (format[i] == 'b')
			{
				configureBoldLayer(lines[i].nextLayer);
			}
			else
			{
				configureLightLayer(lines[i].nextLayer);
			}
		}
		else
		{
			break;
		}
	}
	numLines = i;

	// Calculate y position of top Line within reserved vertical area
	int top_reserve = TOP_TEXT_RESERVE - 7;
	int bottom_reserve = BOTTOM_TEXT_RESERVE;
	int available_height = SCREEN_HEIGHT - top_reserve - bottom_reserve;
	
	// Use tighter row spacing if 4 lines need to fit
	int row_height = ROW_HEIGHT;
	if (numLines == 4) {
		// Calculate required row height to fit all lines
		// total_height = (numLines - 1) * row_height + TEXT_LAYER_HEIGHT
		// We need total_height <= available_height
		// (numLines - 1) * row_height <= available_height - TEXT_LAYER_HEIGHT
		int max_row_height = (available_height - TEXT_LAYER_HEIGHT) / (numLines - 1) + 4;
		if (max_row_height < row_height) {
			row_height = max_row_height;
		}
		top_reserve -= 5;
	}
	
	const int total_height = numLines > 0 ? ((numLines - 1) * row_height + TEXT_LAYER_HEIGHT) : 0;
	int ypos = top_reserve;
	if (total_height < available_height) {
		ypos += (available_height - total_height) / 2;
	}

	// Set y positions for the lines
	for (int i = 0; i < numLines; i++)
	{
		layer_set_frame((Layer *)lines[i].nextLayer, GRect(144, ypos, 144, TEXT_LAYER_HEIGHT));
		ypos += row_height;
	}

	return numLines;
}

static void time_to_lines(int hours, int minutes, int seconds, char lines[NUM_LINES][BUFFER_SIZE], char format[])
{
	int length = NUM_LINES * BUFFER_SIZE + 1;
	char timeStr[length];
	time_to_words(lang, hours, minutes, seconds, timeStr, length);
	
	// Empty all lines
	for (int i = 0; i < NUM_LINES; i++)
	{
		lines[i][0] = '\0';
	}

	char *start = timeStr;
	char *end = strstr(start, " ");
	int l = 0;
	while (end != NULL && l < NUM_LINES) {
		// Check word for bold prefix
		if (*start == '*' && end - start > 1)
		{
			// Mark line bold and move start to the first character of the word
			format[l] = 'b';
			start++;
		}
		else
		{
			// Mark line normal
			format[l] = ' ';
		}

		// Can we add another word to the line?
		if (format[l] == ' ' && *(end + 1) != '*'    // are both lines formatted normal?
			&& end - start < LINE_APPEND_LIMIT - 1)  // is the first word is short enough?
		{
			// See if next word fits
			char *try = strstr(end + 1, " ");
			if (try != NULL && try - start <= LINE_APPEND_LIMIT)
			{
				end = try;
			}
		}

		// copy to line
		*end = '\0';
		strcpy(lines[l++], start);

		// Look for next word
		start = end + 1;
		end = strstr(start, " ");
	}
	
}

// Make a date string
static void date_to_lines(int day, int date, int month, char lines[NUM_LINES][BUFFER_SIZE], char format[]) {
  int length = NUM_LINES * BUFFER_SIZE + 1;
  char dateStr[length];
  
  // Empty all lines
	for (int i = 0; i < NUM_LINES; i++)
	{
		lines[i][0] = '\0';
    format[i] = ' ';
	}
  format[0] = 'b';
  
  date_to_words(lang, day, date, month, dateStr, length);
  
  char *start = dateStr;
	char *end = strstr(start, " ");
	int l = 0;
	while (end != NULL && l < NUM_LINES) {
    // See if next word fits
    char *try = strstr(end + 1, " ");
    if (try != NULL && try - start <= LINE_APPEND_LIMIT)
    {
      end = try;
    }

		// copy to line
		*end = '\0';
		strcpy(lines[l++], start);

		// Look for next word
		start = end + 1;
		end = strstr(start, " ");
	}
}

// Update screen based on new time
static void display_time(struct tm *tm)
{

  // The current time text will be stored in the following strings
  char textLine[NUM_LINES][BUFFER_SIZE];
  char format[NUM_LINES];

	update_top_time_buffer(t);
	update_bottom_status(t);
  
  if (showTime || dateTimeout > 1) {
  	time_to_lines(t->tm_hour, t->tm_min, t->tm_sec, textLine, format);
    dateTimeout = 0;
    showTime = true;
  } else {
    date_to_lines(t->tm_wday, t->tm_mday, t->tm_mon, textLine, format);
  }
  
  int nextNLines = configureLayersForText(textLine, format);

  int delay = 0;
  for (int i = 0; i < NUM_LINES; i++) {
    if (nextNLines != currentNLines || needToUpdateLine(&lines[i], textLine[i])) {
      updateLineTo(&lines[i], textLine[i], delay);
      delay += ANIMATION_STAGGER_TIME;
    }
  }

  currentNLines = nextNLines;
}

static void tap_handler(AccelAxisType axis, int32_t direction)
{
  // Get fresh time data and copy it to our storage
  time_t raw_time;
  time(&raw_time);
  struct tm *temp_time = localtime(&raw_time);
  if (temp_time) {
    current_time = *temp_time;  // Copy the data
  }
  
  showTime = !showTime;
  display_time(t);
}

static void initLineForStart(Line* line)
{
	// Switch current and next layer
	TextLayer* tmp  = line->currentLayer;
	line->currentLayer = line->nextLayer;
	line->nextLayer = tmp;

	// Move current layer to screen;
	GRect rect = layer_get_frame((Layer *)line->currentLayer);
	rect.origin.x = 0;
	layer_set_frame((Layer *)line->currentLayer, rect);
}

// Update screen without animation first time we start the watchface
static void display_initial_time(struct tm *t)
{
	// The current time text will be stored in the following strings
	char textLine[NUM_LINES][BUFFER_SIZE];
	char format[NUM_LINES];

	time_to_lines(t->tm_hour, t->tm_min, t->tm_sec, textLine, format);
	update_top_time_buffer(t);
	update_bottom_status(t);
	
	// Request initial glucose data
	pebble_messenger_request_glucose();

	// Ensure bottom info layers are marked dirty to be rendered
	if (bottom_date_layer) {
		layer_mark_dirty(text_layer_get_layer(bottom_date_layer));
	}
	if (bottom_info_layer) {
		layer_mark_dirty(text_layer_get_layer(bottom_info_layer));
	}
	if (bottom_arrow_layer) {
		layer_mark_dirty(bottom_arrow_layer);
	}

	// This configures the nextLayer for each line
	currentNLines = configureLayersForText(textLine, format);

	// Set the text and configure layers to the start position
	for (int i = 0; i < currentNLines; i++)
	{
		updateLayerText(&lines[i], lines[i].nextLayer, textLine[i]);
		// This call switches current- and nextLayer
		initLineForStart(&lines[i]);
	}	
}

// Time handler called every minute by the system
static void handle_minute_tick(struct tm *tick_time, TimeUnits units_changed)
{
	// Copy time data to our storage (tick_time points to static buffer that can be overwritten)
	if (tick_time) {
		current_time = *tick_time;
	}
  
  if (!showTime) {
    dateTimeout++;
  }
  
	display_time(t);
	
	// Request glucose data every 5 minutes (at 0, 5, 10, 15, etc.)
	if (t->tm_min % 5 == 0) {
		pebble_messenger_request_glucose();
	}
}

/**
 * Debug methods. For quickly debugging enable debug macro on top to transform the watchface into
 * a standard app and you will be able to change the time with the up and down buttons
 */
#if DEBUG

static void up_single_click_handler(ClickRecognizerRef recognizer, Window *window) {
	(void)recognizer;
	(void)window;
	
	t->tm_min += 5;
	if (t->tm_min >= 60) {
		t->tm_min = 0;
		t->tm_hour += 1;
		
		if (t->tm_hour >= 24) {
			t->tm_hour = 0;
		}
	}
	display_time(t);
}


static void down_single_click_handler(ClickRecognizerRef recognizer, Window *window) {
	(void)recognizer;
	(void)window;
	
	t->tm_min -= 5;
	if (t->tm_min < 0) {
		t->tm_min = 55;
		t->tm_hour -= 1;
		
		if (t->tm_hour < 0) {
			t->tm_hour = 23;
		}
	}
	display_time(t);
}

static void click_config_provider(ClickConfig **config, Window *window) {
  (void)window;

  config[BUTTON_ID_UP]->click.handler = (ClickHandler) up_single_click_handler;
  config[BUTTON_ID_UP]->click.repeat_interval_ms = 100;

  config[BUTTON_ID_DOWN]->click.handler = (ClickHandler) down_single_click_handler;
  config[BUTTON_ID_DOWN]->click.repeat_interval_ms = 100;
}

#endif

static void sync_error_callback(DictionaryResult dict_error, AppMessageResult app_message_error, void *context)
{
	APP_LOG(APP_LOG_LEVEL_DEBUG, "App Message Sync Error: %d", app_message_error);
}

static void sync_tuple_changed_callback(const uint32_t key, const Tuple* new_tuple, const Tuple* old_tuple, void* context) {
    GTextAlignment alignment;
    
    // Handle TEXT_ALIGN_KEY
    if (key == TEXT_ALIGN_KEY) {
        text_align = new_tuple->value->uint8;
        persist_write_int(TEXT_ALIGN_KEY, text_align);
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Set text alignment: %u", text_align);

        alignment = lookup_text_alignment(text_align);
        for (int i = 0; i < NUM_LINES; i++)
        {
            text_layer_set_text_alignment(lines[i].currentLayer, alignment);
            text_layer_set_text_alignment(lines[i].nextLayer, alignment);
            layer_mark_dirty(text_layer_get_layer(lines[i].currentLayer));
            layer_mark_dirty(text_layer_get_layer(lines[i].nextLayer));
        }
        if (t) {
            update_top_time_buffer(t);
            update_bottom_status(t);
        }
        if (top_info_layer) {
            layer_mark_dirty(top_info_layer);
        }
        if (bottom_date_layer) {
            layer_mark_dirty(text_layer_get_layer(bottom_date_layer));
        }
        if (bottom_info_layer) {
            layer_mark_dirty(text_layer_get_layer(bottom_info_layer));
        }
        if (bottom_arrow_layer) {
            layer_mark_dirty(bottom_arrow_layer);
        }
    }
    // Handle INVERT_KEY
    else if (key == INVERT_KEY) {
        invert = new_tuple->value->uint8 == 1;
        persist_write_bool(INVERT_KEY, invert);
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Set invert: %u", invert ? 1 : 0);
        
        // Update text layer colors for all lines
        GColor text_color = invert ? GColorBlack : GColorWhite;
        for (int i = 0; i < NUM_LINES; i++) {
            text_layer_set_text_color(lines[i].currentLayer, text_color);
            text_layer_set_text_color(lines[i].nextLayer, text_color);
            layer_mark_dirty(text_layer_get_layer(lines[i].currentLayer));
            layer_mark_dirty(text_layer_get_layer(lines[i].nextLayer));
        }
        
        if (inverter_layer) {
            layer_set_hidden(inverter_layer, !invert);
            layer_mark_dirty(inverter_layer);
        }
        if (t) {
            update_top_time_buffer(t);
            update_bottom_status(t);
        }
        if (top_info_layer) {
            layer_mark_dirty(top_info_layer);
        }
        apply_bottom_theme();
        if (bottom_date_layer) {
            layer_mark_dirty(text_layer_get_layer(bottom_date_layer));
        }
        if (bottom_info_layer) {
            layer_mark_dirty(text_layer_get_layer(bottom_info_layer));
        }
        if (bottom_arrow_layer) {
            layer_mark_dirty(bottom_arrow_layer);
        }
    }
    // Handle LANGUAGE_KEY
    else if (key == LANGUAGE_KEY) {
        lang = (Language) new_tuple->value->uint8;
        persist_write_int(LANGUAGE_KEY, lang);
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Set language: %u", lang);

        if (t)
        {
            display_time(t);
        }
    }
}

// Callback for settings received directly from phone (bypasses AppSync)
// This is called by the messenger when settings arrive
static void settings_received_callback(uint32_t key, int value) {
    GTextAlignment alignment;
    
    APP_LOG(APP_LOG_LEVEL_INFO, "Settings received: key=%lu, value=%d", (unsigned long)key, value);
    
    // Handle TEXT_ALIGN_KEY
    if (key == TEXT_ALIGN_KEY) {
        text_align = value;
        persist_write_int(TEXT_ALIGN_KEY, text_align);
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Set text alignment: %d", text_align);

        alignment = lookup_text_alignment(text_align);
        for (int i = 0; i < NUM_LINES; i++)
        {
            text_layer_set_text_alignment(lines[i].currentLayer, alignment);
            text_layer_set_text_alignment(lines[i].nextLayer, alignment);
            layer_mark_dirty(text_layer_get_layer(lines[i].currentLayer));
            layer_mark_dirty(text_layer_get_layer(lines[i].nextLayer));
        }
        if (t) {
            update_top_time_buffer(t);
            update_bottom_status(t);
        }
        if (top_info_layer) {
            layer_mark_dirty(top_info_layer);
        }
        if (bottom_date_layer) {
            layer_mark_dirty(text_layer_get_layer(bottom_date_layer));
        }
        if (bottom_info_layer) {
            layer_mark_dirty(text_layer_get_layer(bottom_info_layer));
        }
        if (bottom_arrow_layer) {
            layer_mark_dirty(bottom_arrow_layer);
        }
    }
    // Handle INVERT_KEY
    else if (key == INVERT_KEY) {
        invert = (value == 1);
        persist_write_bool(INVERT_KEY, invert);
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Set invert: %u", invert ? 1 : 0);
        
        // Update text layer colors for all lines
        GColor text_color = invert ? GColorBlack : GColorWhite;
        for (int i = 0; i < NUM_LINES; i++) {
            text_layer_set_text_color(lines[i].currentLayer, text_color);
            text_layer_set_text_color(lines[i].nextLayer, text_color);
            layer_mark_dirty(text_layer_get_layer(lines[i].currentLayer));
            layer_mark_dirty(text_layer_get_layer(lines[i].nextLayer));
        }
        
        if (inverter_layer) {
            layer_set_hidden(inverter_layer, !invert);
            layer_mark_dirty(inverter_layer);
        }
        if (t) {
            update_top_time_buffer(t);
            update_bottom_status(t);
        }
        if (top_info_layer) {
            layer_mark_dirty(top_info_layer);
        }
        apply_bottom_theme();
        if (bottom_date_layer) {
            layer_mark_dirty(text_layer_get_layer(bottom_date_layer));
        }
        if (bottom_info_layer) {
            layer_mark_dirty(text_layer_get_layer(bottom_info_layer));
        }
        if (bottom_arrow_layer) {
            layer_mark_dirty(bottom_arrow_layer);
        }
    }
    // Handle LANGUAGE_KEY
    else if (key == LANGUAGE_KEY) {
        lang = (Language) value;
        persist_write_int(LANGUAGE_KEY, lang);
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Set language: %d", lang);

        if (t)
        {
            display_time(t);
        }
    }
}

static void init_line(Line* line)
{
	// Create layers with dummy position to the right of the screen
	line->currentLayer = text_layer_create(GRect(144, 0, 144, 50));
	line->nextLayer = text_layer_create(GRect(144, 0, 144, 50));

	// Configure a style
	configureLightLayer(line->currentLayer);
	configureLightLayer(line->nextLayer);

	// Set the text buffers
	line->lineStr1[0] = '\0';
	line->lineStr2[0] = '\0';
	text_layer_set_text(line->currentLayer, line->lineStr1);
	text_layer_set_text(line->nextLayer, line->lineStr2);

	// Initially there are no animations
	line->animation1 = NULL;
	line->animation2 = NULL;
}

static void destroy_line(Line* line)
{
	// Free layers
	text_layer_destroy(line->currentLayer);
	text_layer_destroy(line->nextLayer);
}



static void window_load(Window *window)
{
	Layer *window_layer = window_get_root_layer(window);
	GRect bounds = layer_get_frame(window_layer);

	// Init and load lines
	for (int i = 0; i < NUM_LINES; i++)
	{
		init_line(&lines[i]);
		layer_add_child(window_layer, (Layer *)lines[i].currentLayer);
		layer_add_child(window_layer, (Layer *)lines[i].nextLayer);
	}

	inverter_layer = layer_create(bounds);
	layer_set_hidden(inverter_layer, !invert);
	layer_set_update_proc(inverter_layer, inverter_update_proc);
	layer_add_child(window_layer, inverter_layer);

	top_info_layer = layer_create(GRect(0, 0, bounds.size.w, TOP_TEXT_RESERVE));
	layer_set_update_proc(top_info_layer, top_info_update_proc);
	layer_add_child(window_layer, top_info_layer);
	layer_mark_dirty(top_info_layer);

	const int bottom_y = SCREEN_HEIGHT - BOTTOM_TEXT_RESERVE;
	bottom_info_background_layer = layer_create(GRect(0, bottom_y, bounds.size.w, BOTTOM_TEXT_RESERVE));
	layer_set_update_proc(bottom_info_background_layer, bottom_info_background_update_proc);
	layer_add_child(window_layer, bottom_info_background_layer);

	const int bottom_text_height = 17;
	const int bottom_text_y = bottom_y + (BOTTOM_TEXT_RESERVE - bottom_text_height) / 2 - 4;
	bottom_date_layer = text_layer_create(GRect(4, bottom_text_y, bounds.size.w / 2, bottom_text_height));
	text_layer_set_background_color(bottom_date_layer, GColorClear);
	text_layer_set_font(bottom_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
	text_layer_set_text_alignment(bottom_date_layer, GTextAlignmentLeft);
	layer_set_clips(text_layer_get_layer(bottom_date_layer), false);
	layer_add_child(window_layer, text_layer_get_layer(bottom_date_layer));

	const int arrow_x = bounds.size.w - BOTTOM_ARROW_WIDTH - 4;
	GRect info_frame = GRect(bounds.size.w / 2, bottom_text_y, arrow_x - (bounds.size.w / 2) - 4, bottom_text_height);
	if (info_frame.size.w < 10) {
		info_frame.size.w = 10;
	}
	bottom_info_layer = text_layer_create(info_frame);
	text_layer_set_background_color(bottom_info_layer, GColorClear);
	text_layer_set_font(bottom_info_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
	text_layer_set_text_alignment(bottom_info_layer, GTextAlignmentRight);
	layer_set_clips(text_layer_get_layer(bottom_info_layer), false);
	layer_add_child(window_layer, text_layer_get_layer(bottom_info_layer));

	bottom_arrow_layer = layer_create(GRect(arrow_x, bottom_y, BOTTOM_ARROW_WIDTH, BOTTOM_TEXT_RESERVE));
	layer_set_update_proc(bottom_arrow_layer, bottom_arrow_update_proc);
	layer_add_child(window_layer, bottom_arrow_layer);

	apply_bottom_theme();



	// Configure time on init - copy to our storage
	time_t raw_time;

	time(&raw_time);
	struct tm *temp_time = localtime(&raw_time);
	if (temp_time) {
		current_time = *temp_time;
	}

	display_initial_time(t);

	// Load persisted settings before AppSync init
	if (persist_exists(TEXT_ALIGN_KEY)) {
		text_align = persist_read_int(TEXT_ALIGN_KEY);
	}
	if (persist_exists(INVERT_KEY)) {
		invert = persist_read_bool(INVERT_KEY);
	}
	if (persist_exists(LANGUAGE_KEY)) {
		lang = persist_read_int(LANGUAGE_KEY);
	}

	Tuplet initial_values[] = {
		TupletInteger(TEXT_ALIGN_KEY, text_align),        // Persistierter Wert
		TupletInteger(INVERT_KEY, invert ? 1 : 0),       // Persistierter Wert
		TupletInteger(LANGUAGE_KEY, lang)                // Persistierter Wert
	};

	app_sync_init(&sync, sync_buffer, sizeof(sync_buffer), initial_values, ARRAY_LENGTH(initial_values),
			sync_tuple_changed_callback, sync_error_callback, NULL);

	// Re-register our handlers AFTER AppSync init, so we intercept messages first
	// and can forward to AppSync if needed. This is critical for settings to work.
	pebble_messenger_register_handlers();
}

static void window_unload(Window *window)
{
	app_sync_deinit(&sync);

	// Free layers
	if (inverter_layer) {
		layer_destroy(inverter_layer);
		inverter_layer = NULL;
	}

	if (top_info_layer) {
		layer_destroy(top_info_layer);
		top_info_layer = NULL;
	}

	if (bottom_info_background_layer) {
		layer_destroy(bottom_info_background_layer);
		bottom_info_background_layer = NULL;
	}

	if (bottom_date_layer) {
		text_layer_destroy(bottom_date_layer);
		bottom_date_layer = NULL;
	}

	if (bottom_info_layer) {
		text_layer_destroy(bottom_info_layer);
		bottom_info_layer = NULL;
	}

	if (bottom_arrow_layer) {
		layer_destroy(bottom_arrow_layer);
		bottom_arrow_layer = NULL;
	}

	for (int i = 0; i < NUM_LINES; i++)
	{
		destroy_line(&lines[i]);
	}
}

static void handle_init() {
	// Settings loaded in window_load before AppSync init

	// Get current time immediately for buffer initialization and copy to our storage
	time_t raw_time;
	time(&raw_time);
	struct tm *init_time = localtime(&raw_time);
	if (init_time) {
		current_time = *init_time;  // Copy to our storage
	}
	
	// Initialize time buffer with actual current time (not placeholder)
	const char *format = clock_is_24h_style() ? "%H:%M" : "%I:%M";
	if (strftime(top_time_buffer, sizeof(top_time_buffer), format, t) == 0) {
		snprintf(top_time_buffer, sizeof(top_time_buffer), "--:--");
	}
	if (!clock_is_24h_style() && top_time_buffer[0] == '0') {
		memmove(top_time_buffer, top_time_buffer + 1, sizeof(top_time_buffer) - 1);
	}
	
	// Initialize date buffer with actual current date
	if (strftime(bottom_date_buffer, sizeof(bottom_date_buffer), "%d.%m.%Y", t) == 0) {
		snprintf(bottom_date_buffer, sizeof(bottom_date_buffer), "--.--.----");
	}
	
	// Glucose info starts empty until we receive data
	snprintf(bottom_info_buffer, sizeof(bottom_info_buffer), "---");
	bottom_trend_direction = TREND_UNKNOWN;
	
	current_battery_state = battery_state_service_peek();
	bluetooth_connected = connection_service_peek_pebble_app_connection();
	battery_state_service_subscribe(battery_state_handler);
	connection_service_subscribe((ConnectionHandlers) {
		.pebble_app_connection_handler = bluetooth_handler
	});

	// Initialize messenger with callbacks for receiving glucose data and settings
	// Note: Must be called BEFORE app_message_open()
	pebble_messenger_init(glucose_data_received_callback, settings_received_callback);

	window = window_create();
	window_set_background_color(window, GColorBlack);
	window_set_window_handlers(window, (WindowHandlers) {
		.load = window_load,
		.unload = window_unload
	});

	// Open app message channel - larger buffers for glucose data
	// Note: Only call once, messenger_init registers callbacks but doesn't open
	const int inbound_size = 256;
	const int outbound_size = 128;
	app_message_open(inbound_size, outbound_size);

	const bool animated = true;
	window_stack_push(window, animated);
  
  // Sample as little as often to save battery and no need for precision
  accel_service_set_sampling_rate(ACCEL_SAMPLING_10HZ);
  accel_tap_service_subscribe(tap_handler);

	// Subscribe to minute ticks
	tick_timer_service_subscribe(MINUTE_UNIT, handle_minute_tick);

#if DEBUG
	// Button functionality
	window_set_click_config_provider(window, (ClickConfigProvider) click_config_provider);
#endif
}

static void handle_deinit()
{
	tick_timer_service_unsubscribe();
	accel_tap_service_unsubscribe();
	connection_service_unsubscribe();
	battery_state_service_unsubscribe();
	pebble_messenger_deinit();
	
	// Clear animation pointers (animations auto-destroy when finished)
	for (int i = 0; i < NUM_LINES; i++) {
		lines[i].animation1 = NULL;
		lines[i].animation2 = NULL;
	}
	
	// Free window
	window_destroy(window);
}

int main(void)
{
	handle_init();
	app_event_loop();
	handle_deinit();
}

