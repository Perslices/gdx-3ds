// tools/dev/mac/wid.swift — window ids for on-screen windows, filtered by owner name.
//
// Vendored replacement for the /tmp/wid.swift every agent rewrote and lost
// (docs/research/RESUME-2026-09-05.md:25, locked60-round3.md:6). `screencapture -l <id>`
// needs a CGWindowID, and there is no shell command that yields one.
//
//   swift tools/dev/mac/wid.swift [owner-substring]     # default: azahar
//
// Prints one line per window: <id> <ownerPID> <width>x<height> <owner> :: <title>
// Exit 1 if no window matched (the caller then falls back to a full-screen capture).

import Foundation
import CoreGraphics

let needle = (CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "azahar").lowercased()
let opts: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
guard let windows = CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: AnyObject]] else {
    FileHandle.standardError.write("cannot read the window list\n".data(using: .utf8)!)
    exit(2)
}

// Largest window first. A modal alert ("The `azahar` executable is being run directly...")
// is a real, capturable window of the same owner and would otherwise win by z-order,
// making `emu shot` return a picture of the dialog. Measured 2026-09-07.
struct Hit { let wid: Int; let pid: Int; let w: Int; let h: Int; let owner: String; let title: String }
var hits: [Hit] = []
for w in windows {
    let owner = (w[kCGWindowOwnerName as String] as? String ?? "")
    let title = (w[kCGWindowName as String] as? String ?? "")
    guard owner.lowercased().contains(needle) || title.lowercased().contains(needle) else { continue }
    let wid = (w[kCGWindowNumber as String] as? Int) ?? -1
    let pid = (w[kCGWindowOwnerPID as String] as? Int) ?? -1
    var width = 0, height = 0
    if let b = w[kCGWindowBounds as String] as? [String: CGFloat] {
        width = Int(b["Width"] ?? 0); height = Int(b["Height"] ?? 0)
    }
    // Skip the 1x1/menu shims some apps keep around; a real emulator window is big.
    if width < 64 || height < 64 { continue }
    hits.append(Hit(wid: wid, pid: pid, w: width, h: height, owner: owner, title: title))
}

hits.sort { ($0.w * $0.h) > ($1.w * $1.h) }
for h in hits {
    print("\(h.wid) \(h.pid) \(h.w)x\(h.h) \(h.owner) :: \(h.title)")
}
exit(hits.isEmpty ? 1 : 0)
