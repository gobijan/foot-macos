#import "view.h"

#import "app.h"

#include <stdlib.h>

#include <tllist.h>

#define LOG_MODULE "accessibility"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "composed.h"
#include "grid.h"
#include "selection.h"
#include "terminal.h"
#include "util.h"
#include "xmalloc.h"

/*
 * Accessibility (VoiceOver): the view is a text area, whose text is the
 * visible part of the terminal, one line per row.
 *
 * Its text is only assembled when asked for, and kept until the next
 * frame; without a client, frames just drop the (nonexistent) copy. The
 * client is told about changes at most every 100 ms, as output may come
 * much faster than that can be read out.
 */

#define NOTIFICATION_DELAY .1

/* The text, and where its characters (UTF-16 units) are in the grid */
@interface FootScreenText : NSObject
@property (nonatomic) NSString *string;
@property (nonatomic) NSMutableData *line_starts;   /* NSUInteger, per row */
@property (nonatomic) NSMutableData *columns;       /* uint16_t, per character */
@property (nonatomic) NSUInteger caret;
@end

@implementation FootScreenText
@end

static void
put_char(NSMutableData *text, NSMutableData *columns, char32_t wc, uint16_t col)
{
    unichar units[2];
    size_t count = 1;

    if (wc >= 0x10000) {
        wc -= 0x10000;
        units[0] = 0xd800 + (wc >> 10);
        units[1] = 0xdc00 + (wc & 0x3ff);
        count = 2;
    } else
        units[0] = wc;

    [text appendBytes:units length:count * sizeof(units[0])];
    for (size_t i = 0; i < count; i++)
        [columns appendBytes:&col length:sizeof(col)];
}

static void
put_cell(NSMutableData *text, NSMutableData *columns, const struct terminal *term,
         const struct cell *cell, uint16_t col)
{
    const char32_t wc = cell->wc;

    /* The other half of a wide character */
    if (wc >= CELL_SPACER)
        return;

    if (wc >= CELL_COMB_CHARS_LO && wc <= CELL_COMB_CHARS_HI) {
        const struct composed *composed = composed_lookup(
            term->composed, wc - CELL_COMB_CHARS_LO);

        for (size_t i = 0; composed != NULL && i < composed->count; i++)
            put_char(text, columns, composed->chars[i], col);
    } else
        put_char(text, columns, wc != 0 && wc != U'\t' ? wc : U' ', col);
}

/* The cursor's row in the view, or -1 if it's scrolled out of it */
static int
cursor_row_in_view(const struct terminal *term)
{
    const struct grid *grid = term->grid;
    const int row = (grid->offset + grid->cursor.point.row - grid->view) & (grid->num_rows - 1);
    return row < term->rows ? row : -1;
}

static FootScreenText *
screen_text_create(struct terminal *term)
{
    FootScreenText *screen = [[FootScreenText alloc] init];
    NSMutableData *text = [NSMutableData dataWithCapacity:term->rows * (term->cols + 1) * sizeof(unichar)];

    screen.line_starts = [NSMutableData dataWithLength:term->rows * sizeof(NSUInteger)];
    screen.columns = [NSMutableData dataWithCapacity:term->rows * (term->cols + 1) * sizeof(uint16_t)];

    const int cursor_row = cursor_row_in_view(term);
    const int cursor_col = term->grid->cursor.point.col;
    NSUInteger *line_starts = screen.line_starts.mutableBytes;

    for (int r = 0; r < term->rows; r++) {
        const struct row *row = grid_row_in_view(term->grid, r);
        line_starts[r] = text.length / sizeof(unichar);

        /* Without trailing blanks */
        int end = term->cols;
        while (end > 0 && (row->cells[end - 1].wc == 0 || row->cells[end - 1].wc == U' '))
            end--;

        for (int col = 0; col < end; col++) {
            if (r == cursor_row && col == cursor_col)
                screen.caret = text.length / sizeof(unichar);
            put_cell(text, screen.columns, term, &row->cells[col], col);
        }

        if (r == cursor_row && cursor_col >= end)
            screen.caret = text.length / sizeof(unichar);

        put_char(text, screen.columns, U'\n', end);
    }

    screen.string = [[NSString alloc] initWithBytes:text.bytes
                                             length:text.length
                                           encoding:NSUTF16LittleEndianStringEncoding];
    return screen;
}

@implementation FootView (Accessibility)

- (FootScreenText *)screenText
{
    struct terminal *term = self.term;
    if (term == NULL || term->grid == NULL || term->rows == 0)
        return nil;

    if (self.accessibilityText == nil)
        self.accessibilityText = screen_text_create(term);
    return self.accessibilityText;
}

- (void)postAccessibilityChange
{
    self.accessibilityPending = NO;
    NSAccessibilityPostNotification(self, NSAccessibilityValueChangedNotification);
    NSAccessibilityPostNotification(self, NSAccessibilitySelectedTextChangedNotification);
}

- (void)accessibilityFrameRendered
{
    self.accessibilityText = nil;

    if (self.accessibilityPending || !NSWorkspace.sharedWorkspace.isVoiceOverEnabled)
        return;

    self.accessibilityPending = YES;
    [self performSelector:@selector(postAccessibilityChange)
               withObject:nil
               afterDelay:NOTIFICATION_DELAY];
}

- (void)accessibilityInvalidate
{
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
    self.accessibilityText = nil;
}

- (BOOL)isAccessibilityElement
{
    return YES;
}

- (NSAccessibilityRole)accessibilityRole
{
    return NSAccessibilityTextAreaRole;
}

- (NSString *)accessibilityRoleDescription
{
    return @"terminal";
}

- (NSString *)accessibilityLabel
{
    return self.window.title;
}

- (id)accessibilityValue
{
    return self.screenText.string;
}

- (NSInteger)accessibilityNumberOfCharacters
{
    return self.screenText.string.length;
}

- (NSString *)accessibilitySelectedText
{
    struct terminal *term = self.term;
    char *text = term != NULL ? selection_to_text(term) : NULL;
    if (text == NULL)
        return @"";

    NSString *string = @(text);
    free(text);
    return string != nil ? string : @"";
}

- (NSRange)accessibilitySelectedTextRange
{
    return NSMakeRange(self.screenText.caret, 0);
}

- (NSRange)accessibilityVisibleCharacterRange
{
    return NSMakeRange(0, self.screenText.string.length);
}

- (NSInteger)accessibilityInsertionPointLineNumber
{
    return [self accessibilityLineForIndex:self.screenText.caret];
}

- (NSInteger)accessibilityLineForIndex:(NSInteger)index
{
    FootScreenText *screen = self.screenText;
    if (screen == nil)
        return 0;

    const NSUInteger *line_starts = screen.line_starts.bytes;
    const size_t count = screen.line_starts.length / sizeof(line_starts[0]);

    size_t line = 0;
    while (line + 1 < count && line_starts[line + 1] <= (NSUInteger)index)
        line++;
    return line;
}

- (NSRange)accessibilityRangeForLine:(NSInteger)line
{
    FootScreenText *screen = self.screenText;
    if (screen == nil)
        return NSMakeRange(NSNotFound, 0);

    const NSUInteger *line_starts = screen.line_starts.bytes;
    const size_t count = screen.line_starts.length / sizeof(line_starts[0]);
    if (line < 0 || (size_t)line >= count)
        return NSMakeRange(NSNotFound, 0);

    const NSUInteger end = (size_t)line + 1 < count ? line_starts[line + 1] : screen.string.length;
    return NSMakeRange(line_starts[line], end - line_starts[line]);
}

- (NSRange)accessibilityRangeForIndex:(NSInteger)index
{
    NSString *string = self.screenText.string;
    return (NSUInteger)index < string.length
        ? [string rangeOfComposedCharacterSequenceAtIndex:index]
        : NSMakeRange(NSNotFound, 0);
}

- (NSString *)accessibilityStringForRange:(NSRange)range
{
    NSString *string = self.screenText.string;
    return NSMaxRange(range) <= string.length ? [string substringWithRange:range] : nil;
}

- (NSAttributedString *)accessibilityAttributedStringForRange:(NSRange)range
{
    NSString *string = [self accessibilityStringForRange:range];
    return string != nil ? [[NSAttributedString alloc] initWithString:string] : nil;
}

/* In screen coordinates; only of the range's first line */
- (NSRect)accessibilityFrameForRange:(NSRange)range
{
    FootScreenText *screen = self.screenText;
    struct terminal *term = self.term;
    if (screen == nil || NSMaxRange(range) > screen.string.length)
        return NSZeroRect;

    const uint16_t *columns = screen.columns.bytes;
    const NSInteger line = [self accessibilityLineForIndex:range.location];
    const NSRange line_range = [self accessibilityRangeForLine:line];

    const NSUInteger last = MIN(NSMaxRange(range), NSMaxRange(line_range)) - 1;
    const int start_col = range.location < screen.string.length ? columns[range.location] : 0;
    const int end_col = range.length > 0 ? columns[MAX(last, range.location)] + 1 : start_col + 1;

    /* Points */
    const float scale = term->scale;
    const NSRect rect = NSMakeRect(
        (term->margins.left + start_col * term->cell_width) / scale,
        (term->margins.top + line * term->cell_height) / scale,
        (end_col - start_col) * term->cell_width / scale,
        term->cell_height / scale);

    return [self.window convertRectToScreen:[self convertRect:rect toView:nil]];
}

- (NSRange)accessibilityRangeForPosition:(NSPoint)point
{
    FootScreenText *screen = self.screenText;
    struct terminal *term = self.term;
    if (screen == nil)
        return NSMakeRange(NSNotFound, 0);

    const NSPoint window = [self.window convertPointFromScreen:point];
    const NSPoint p = [self convertPoint:window fromView:nil];

    const float scale = term->scale;
    const int col = (p.x * scale - term->margins.left) / term->cell_width;
    const int row = (p.y * scale - term->margins.top) / term->cell_height;

    const NSRange line = [self accessibilityRangeForLine:MIN(MAX(row, 0), term->rows - 1)];
    if (line.location == NSNotFound)
        return line;

    const uint16_t *columns = screen.columns.bytes;
    for (NSUInteger i = line.location; i < NSMaxRange(line); i++) {
        if (columns[i] >= col)
            return [self accessibilityRangeForIndex:i];
    }
    return NSMakeRange(NSMaxRange(line) - 1, 1);
}

@end
