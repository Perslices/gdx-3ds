// tools/dev/mac/click.swift — one synthetic mouse click in global screen coordinates.
//
// Vendored replacement for the hand-written CGEvent snippet each agent re-derived
// (05-devcli-plan.md §2 item 9). Used to drive Azahar's bottom-screen touch menu, which
// autoinput cannot reach (the menu is host-side UI, not guest input).
//
//   swift tools/dev/mac/click.swift <x> <y> [--double] [--move-only]
//
// Requires Accessibility permission for the terminal app; without it the events are
// silently dropped, so the caller should verify the effect rather than the exit code.

import Foundation
import CoreGraphics

let args = CommandLine.arguments
guard args.count >= 3, let x = Double(args[1]), let y = Double(args[2]) else {
    FileHandle.standardError.write("usage: click.swift <x> <y> [--double] [--move-only]\n".data(using: .utf8)!)
    exit(2)
}
let double = args.contains("--double")
let moveOnly = args.contains("--move-only")
let pt = CGPoint(x: x, y: y)
let src = CGEventSource(stateID: .hidSystemState)

func post(_ type: CGEventType, clicks: Int64) {
    guard let e = CGEvent(mouseEventSource: src, mouseType: type,
                          mouseCursorPosition: pt, mouseButton: .left) else { return }
    e.setIntegerValueField(.mouseEventClickState, value: clicks)
    e.post(tap: .cghidEventTap)
    usleep(30_000)
}

post(.mouseMoved, clicks: 0)
if !moveOnly {
    post(.leftMouseDown, clicks: 1)
    post(.leftMouseUp, clicks: 1)
    if double {
        post(.leftMouseDown, clicks: 2)
        post(.leftMouseUp, clicks: 2)
    }
}
print("clicked \(Int(x)),\(Int(y))\(double ? " (double)" : "")\(moveOnly ? " (move only)" : "")")
