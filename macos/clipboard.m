#import <AppKit/AppKit.h>

#include <stdlib.h>
#include <string.h>

#include "wayland.h"

/*
 * The clipboard is the general pasteboard. Files (e.g. copied in
 * Finder) are pasted as their paths, like dropped files.
 */

long
wayl_clipboard_set(struct wayland *wayl, const char *text)
{
    NSString *str = [NSString stringWithUTF8String:text];
    if (str == nil)
        return -1;

    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];

    if (![pb setString:str forType:NSPasteboardTypeString])
        return -1;

    return pb.changeCount;
}

void
wayl_clipboard_clear(struct wayland *wayl, long change_count)
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];

    /* Only if it still holds what we put there */
    if (pb.changeCount == change_count)
        [pb clearContents];
}

static NSArray<NSURL *> *
file_urls(NSPasteboard *pb)
{
    return [pb readObjectsForClasses:@[[NSURL class]]
                             options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
}

bool
wayl_clipboard_has_data(const struct wayland *wayl)
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    return [pb availableTypeFromArray:@[NSPasteboardTypeString, NSPasteboardTypeFileURL]] != nil;
}

char *
wayl_clipboard_get(struct wayland *wayl, enum data_offer_mime_type *mime_type,
                   size_t *len)
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSString *text = nil;

    NSArray<NSURL *> *urls = file_urls(pb);
    if (urls.count > 0) {
        NSMutableString *list = [NSMutableString string];
        for (NSURL *url in urls)
            [list appendFormat:@"%@\r\n", url.absoluteString];

        text = list;
        *mime_type = DATA_OFFER_MIME_URI_LIST;
    } else {
        text = [pb stringForType:NSPasteboardTypeString];
        *mime_type = DATA_OFFER_MIME_TEXT_UTF8;
    }

    if (text == nil)
        return NULL;

    const char *utf8 = text.UTF8String;
    if (utf8 == NULL)
        return NULL;

    *len = strlen(utf8);
    return strdup(utf8);
}
