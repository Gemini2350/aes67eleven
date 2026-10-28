#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>
#include "menubar.h"
#include "log.h"

void aes67vsc::RequestMicAccess(std::function<void(bool)> onDone)
{
    //audio INPUT from any device - even our own virtual one - is gated by the
    //microphone tcc permission. without it coreaudiod silently serves zeros.
    auto done = std::make_shared<std::function<void(bool)>>(std::move(onDone));
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL granted)
    {
        if(*done) { (*done)(granted); }
    }];
}

namespace
{
    std::function<void()> g_onQuit;
    std::function<std::string()> g_statusFn;
    NSStatusItem* g_pItem = nil;
    NSMenuItem* g_pStatusLine = nil;
}

@interface VscMenuTarget : NSObject
- (void)openUi:(id)sender;
- (void)quit:(id)sender;
@end

@implementation VscMenuTarget
- (void)openUi:(id)sender
{
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"http://localhost:8180"]];
}
- (void)quit:(id)sender
{
    if(g_onQuit) { g_onQuit(); }    //engine winds down and calls StopMenuBar()
}
@end

namespace
{
    void BuildStatusItem()
    {
        static VscMenuTarget* target = [VscMenuTarget new];

        if(g_pItem) { [[NSStatusBar systemStatusBar] removeStatusItem:g_pItem]; }
        g_pItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
        g_pItem.visible = YES;
        NSImage* icon = [[NSBundle mainBundle] imageForResource:@"MenuIcon"];
        if(icon)
        {
            [icon setTemplate:YES];     //adapts to light/dark menu bar
            g_pItem.button.image = icon;
        }
        else
        {
            g_pItem.button.title = @"AES67";   //running outside the app bundle
        }
        g_pItem.button.toolTip = @"AES67eleven - Virtual Soundcard with NMOS support";

        NSMenu* menu = [NSMenu new];
        g_pStatusLine = [menu addItemWithTitle:@"starting…" action:nil keyEquivalent:@""];
        g_pStatusLine.enabled = NO;
        [menu addItem:[NSMenuItem separatorItem]];
        NSMenuItem* open = [menu addItemWithTitle:@"Open web UI" action:@selector(openUi:) keyEquivalent:@"o"];
        open.target = target;
        [menu addItem:[NSMenuItem separatorItem]];
        NSMenuItem* quit = [menu addItemWithTitle:@"Quit AES67eleven" action:@selector(quit:) keyEquivalent:@"q"];
        quit.target = target;
        g_pItem.menu = menu;
    }
}

@interface VscAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation VscAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)note
{
    //macos 26 (tahoe): the menu bar is scene based and a status item created
    //BEFORE the app finished launching may never get its scene activated
    //(appkit retries forever, no icon appears). build it here instead.
    LOG_INFO("menubar") << "app finished launching - creating status item";
    BuildStatusItem();

    [NSTimer scheduledTimerWithTimeInterval:2.0 repeats:YES block:^(NSTimer*)
    {
        if(g_statusFn && g_pStatusLine)
        {
            g_pStatusLine.title = [NSString stringWithUTF8String:g_statusFn().c_str()];
        }
    }];
}
@end

void aes67vsc::RunMenuBar(std::function<void()> onQuit, std::function<std::string()> statusFn)
{
    g_onQuit = std::move(onQuit);
    g_statusFn = std::move(statusFn);

    @autoreleasepool
    {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        static VscAppDelegate* delegate = [VscAppDelegate new];
        NSApp.delegate = delegate;
        [NSApp run];
    }
}

void aes67vsc::StopMenuBar()
{
    dispatch_async(dispatch_get_main_queue(), ^
    {
        [NSApp stop:nil];
        //stop: only takes effect after an event - poke the queue
        NSEvent* poke = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                           location:NSZeroPoint modifierFlags:0 timestamp:0
                                       windowNumber:0 context:nil subtype:0 data1:0 data2:0];
        [NSApp postEvent:poke atStart:YES];
    });
}
