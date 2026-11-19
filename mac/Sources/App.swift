import AppKit
import ApplicationServices
import ServiceManagement

private func label(_ s: String, size: CGFloat = 13, bold: Bool = false, secondary: Bool = false) -> NSTextField {
    let l = NSTextField(wrappingLabelWithString: s)
    l.font = bold ? .boldSystemFont(ofSize: size) : .systemFont(ofSize: size)
    if secondary { l.textColor = .secondaryLabelColor }
    l.preferredMaxLayoutWidth = 480
    return l
}
private func row(_ v: [NSView]) -> NSStackView {
    let s = NSStackView(views: v); s.orientation = .horizontal; s.spacing = 8; s.alignment = .centerY; return s
}
private func line() -> NSBox { let b = NSBox(); b.boxType = .separator; return b }
private func button(_ t: String, _ target: AnyObject, _ sel: Selector) -> NSButton { let b = NSButton(title: t, target: target, action: sel); b.bezelStyle = .rounded; return b }
private func mmss(_ s: Int) -> String { String(format: "%d:%02d", s / 60, s % 60) }

final class MainWindow: NSObject, NSWindowDelegate, NSTextFieldDelegate {
    let core: Core
    var window: NSWindow!
    private let sharing = NSSwitch()
    private let status = label("", secondary: true)
    private let permission = NSButton(title: "Allow Shenzhen KVM to use the keyboard and mouse…", target: nil, action: nil)
    // linking
    private let linkTitle = label("Link with another computer", bold: true)
    private let linkHelp = label("On both computers, open Shenzhen KVM. Make one of them visible, then choose it on the other.", secondary: true)
    private var visibleButton: NSButton!
    private let visibleLine = label("")
    private var stopVisible: NSButton!
    private var visibleRow: NSStackView!
    private let visibleHint = label("", size: 11, secondary: true)
    private let foundTitle = label("Visible computers:")
    private let foundBox = NSStackView()
    private let linkingLine = label("", secondary: true)
    private var linkingCancel: NSButton!
    private var linkingRow: NSStackView!
    private let linkedLine = label("")
    private var forgetButton: NSButton!
    private var linkedRow: NSStackView!
    private var settingsViews: [NSView] = []
    // settings
    private let macDrives = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let peerDrives = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    private let side = NSPopUpButton()
    private let sideLabel = label("")
    private let sideHint = label("", size: 11, secondary: true)
    private let commandLabel = label("")
    private let command = NSPopUpButton()
    private var commandRow: NSStackView!
    private let clipboard = NSButton(checkboxWithTitle: "Shared clipboard: copy on one computer, paste on the other", target: nil, action: nil)
    private let usual = NSButton(checkboxWithTitle: "Use the usual copy and paste (⌘C / ⌘V) between the two computers", target: nil, action: nil)
    private let copyKeyButton = NSButton(title: "", target: nil, action: nil)
    private let pasteKeyButton = NSButton(title: "", target: nil, action: nil)
    private var keyRows: [NSView] = []
    private let bigFiles = NSButton(checkboxWithTitle: "Send copied files of any size", target: nil, action: nil)
    private let limit = NSTextField(string: "100")
    private var limitRow: NSStackView!
    private let notice = label("", secondary: true)
    private let smooth = NSButton(checkboxWithTitle: "Smooth pointer (pauses AirDrop and Handoff while sharing)", target: nil, action: nil)
    private let smoothHint = label("This needs a one-time setup with your Mac's password.", size: 11, secondary: true)
    private var smoothRow: NSStackView!
    private var recorder: Any?
    private let transfersBox = NSStackView()
    private let transfersTitle = label("Clipboard transfers", bold: true)
    private let transfersLine = line()
    private var sheetShownFor = ""
    private var stack: NSStackView!

    init(core: Core) { self.core = core; super.init(); build() }

    private func build() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 540, height: 600), styleMask: [.titled, .closable, .miniaturizable],
                          backing: .buffered, defer: false)
        window.title = "Shenzhen KVM"
        window.isReleasedWhenClosed = false
        window.delegate = self

        for b in [macDrives, peerDrives, clipboard, usual, bigFiles, smooth] { b.target = self; b.action = #selector(changed(_:)) }
        sharing.target = self; sharing.action = #selector(changed(_:))
        permission.target = self; permission.action = #selector(openAX); permission.bezelStyle = .rounded
        side.addItems(withTitles: ["above this Mac", "below this Mac", "to the left of this Mac", "to the right of this Mac"])
        side.target = self; side.action = #selector(changed(_:))
        command.addItems(withTitles: ["Control (⌘C copies)", "Super"])
        command.target = self; command.action = #selector(changed(_:))
        copyKeyButton.target = self; copyKeyButton.action = #selector(record(_:)); copyKeyButton.bezelStyle = .rounded
        pasteKeyButton.target = self; pasteKeyButton.action = #selector(record(_:)); pasteKeyButton.bezelStyle = .rounded
        limit.delegate = self; limit.alignment = .right
        limit.widthAnchor.constraint(equalToConstant: 70).isActive = true
        limitRow = row([label("Files bigger than"), limit, label("MB are not sent.")])
        visibleButton = button("Make this Mac visible", self, #selector(makeVisible))
        stopVisible = button("Stop", self, #selector(stopVisibleNow))
        visibleRow = row([visibleLine, stopVisible])
        linkingCancel = button("Cancel", self, #selector(cancelLinking))
        linkingRow = row([linkingLine, linkingCancel])
        forgetButton = button("", self, #selector(forget))
        linkedRow = row([linkedLine, NSView(), forgetButton])
        foundBox.orientation = .vertical; foundBox.alignment = .leading; foundBox.spacing = 6
        let setupSmooth = button("Set Up…", self, #selector(setUpSmooth))
        smoothRow = row([smoothHint, setupSmooth])

        let title = label("Shenzhen KVM", size: 22, bold: true)
        let head = row([title, NSView(), label("Sharing"), sharing])
        head.setHuggingPriority(.defaultLow, for: .horizontal)
        commandRow = row([commandLabel, command])
        keyRows = [row([label("Copy here and send to the other computer:"), copyKeyButton]),
                   row([label("Paste what the other computer copied:"), pasteKeyButton])]
        settingsViews = [macDrives, peerDrives, row([sideLabel, side]), sideHint, commandRow, line(), clipboard, usual] + keyRows + [bigFiles, limitRow, notice, line()]

        let views: [NSView] = [head, status, permission, line(),
            linkTitle, linkHelp, visibleButton, visibleRow, visibleHint, foundTitle, foundBox, linkingRow, linkedRow, line()]
            + settingsViews + [transfersTitle, transfersBox, transfersLine, smooth, smoothRow, line(),
            label("Press both Shift keys at once to bring the mouse back to this Mac.", secondary: true)]
        transfersBox.orientation = .vertical; transfersBox.alignment = .leading; transfersBox.spacing = 6
        let stack = NSStackView(views: views)
        stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 10
        stack.edgeInsets = NSEdgeInsets(top: 20, left: 24, bottom: 20, right: 24)
        stack.translatesAutoresizingMaskIntoConstraints = false
        stack.distribution = .fill
        stack.setHuggingPriority(.required, for: .vertical)
        self.stack = stack
        for v in [head, linkedRow!] { v.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -48).isActive = true }
        window.contentView = NSView()
        window.contentView!.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: window.contentView!.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: window.contentView!.trailingAnchor),
            stack.topAnchor.constraint(equalTo: window.contentView!.topAnchor),
            stack.bottomAnchor.constraint(equalTo: window.contentView!.bottomAnchor),
            stack.widthAnchor.constraint(equalToConstant: 540),
        ])
        refresh()
        window.center()
    }

    func show() { refresh(); NSApp.activate(ignoringOtherApps: true); window.makeKeyAndOrderFront(nil) }
    func windowWillClose(_ notification: Notification) { core.finder.setLooking(false) }

    func refresh() {
        let (c, m, online, linked, note, os) = core.locked { (core.cfg, core.mode, core.peerOnline, core.link != nil, core.lastNotice, core.peerOS) }
        let other = otherComputer(), Other = OtherComputer()
        // linking
        let lv = core.linker.locked { core.linker.view }
        let visibleLeft = core.finder.visibleLeft
        let showLink = !linked
        core.finder.setLooking(showLink && window.isVisible)
        linkTitle.isHidden = !showLink; linkHelp.isHidden = !showLink
        visibleButton.isHidden = !showLink || visibleLeft > 0
        visibleRow.isHidden = !showLink || visibleLeft == 0
        visibleLine.stringValue = "Visible to computers on this network for \(mmss(visibleLeft))"
        visibleHint.stringValue = "On the other computer, choose “\(core.finder.name)”."
        visibleHint.isHidden = visibleRow.isHidden
        foundTitle.isHidden = !showLink; foundBox.isHidden = !showLink
        if showLink { refreshFound() }
        var linking = ""
        switch lv.stage {
        case .connecting, .waiting: linking = lv.asked ? "" : "Asking \(lv.peerName)…"
        case .confirm: linking = lv.youConfirmed && !lv.theyConfirmed ? "Waiting for \(lv.peerName) to confirm…" : ""
        case .done, .failed: linking = linked && lv.stage == .done ? "" : lv.message
        case .none: break
        }
        linkingLine.stringValue = linking
        linkingRow.isHidden = linking.isEmpty
        linkingCancel.isHidden = !(lv.stage == .connecting || lv.stage == .waiting || lv.stage == .confirm)
        linkedRow.isHidden = !linked
        linkedLine.stringValue = "Linked with \(other)."
        forgetButton.title = "Forget \(other)…"
        for v in settingsViews { v.isHidden = !linked }
        if lv.stage == .confirm && !lv.youConfirmed { askCode(lv) } else if window.attachedSheet != nil && sheetShownFor != "" && lv.stage != .confirm { window.endSheet(window.attachedSheet!); sheetShownFor = "" }

        // settings
        macDrives.title = "This Mac's keyboard and mouse can control \(other)"
        peerDrives.title = "\(Other)'s keyboard and mouse can control this Mac"
        sideLabel.stringValue = "\(Other)'s screen is"
        sideHint.stringValue = "Move the pointer past that side of your screens to go to \(other); it comes back the same way."
        commandLabel.stringValue = "On \(other), this Mac's ⌘ key acts as"
        commandRow.isHidden = !linked || os == "mac"
        command.selectItem(at: c.commandSuper ? 1 : 0)
        sharing.state = c.paused ? .off : .on
        macDrives.state = c.macDrivesPeer ? .on : .off
        peerDrives.state = c.peerDrivesMac ? .on : .off
        side.selectItem(at: [Edge.top, .bottom, .left, .right].firstIndex(of: c.edge) ?? 0)
        clipboard.state = c.clipboard ? .on : .off
        usual.state = c.usualShortcuts ? .on : .off
        copyKeyButton.title = c.copyKey.title; pasteKeyButton.title = c.pasteKey.title
        for r in keyRows { r.isHidden = !linked || c.usualShortcuts || !c.clipboard }
        bigFiles.state = c.bigFiles ? .on : .off
        limitRow.isHidden = !linked || c.bigFiles || !c.clipboard
        if limit.currentEditor() == nil { limit.stringValue = "\(c.fileLimitMB)" }
        notice.stringValue = note; notice.isHidden = !linked || note.isEmpty
        for v in [macDrives, peerDrives, side, command, clipboard, usual, bigFiles] { v.isEnabled = !c.paused && (v !== usual && v !== bigFiles || c.clipboard) }
        refreshTransfers()
        smooth.state = c.smooth ? .on : .off
        smooth.isEnabled = core.awdl.installed
        smoothRow.isHidden = core.awdl.installed
        permission.isHidden = core.tap != nil || inputDisabled
        let s: String
        if core.tap == nil && !inputDisabled { s = "Not working yet: Shenzhen KVM needs your permission to use the keyboard and mouse." }
        else if !linked { s = "Not linked with another computer yet." }
        else if c.paused { s = "Sharing is off: your keyboard, mouse and clipboard stay on this Mac." }
        else if m == .driving { s = "The mouse is on \(other)." }
        else if m == .driven { s = "\(Other)'s keyboard and mouse are in use here." }
        else if online { s = "The mouse is on this Mac. \(Other) is connected." }
        else { s = "The mouse is on this Mac. \(Other) is not answering: check that it is on and on the same network." }
        status.stringValue = s
        fitWindow()
    }
    /// The window follows its content (sections come and go).
    private func fitWindow() {
        stack.layoutSubtreeIfNeeded()
        let h = stack.fittingSize.height
        guard abs(window.contentLayoutRect.height - h) > 0.5 else { return }
        var f = window.frame; let dh = h - window.contentLayoutRect.height
        f.origin.y -= dh; f.size.height += dh
        window.setFrame(f, display: true)
    }

    private var lastFound: [String] = []
    private func refreshFound() {
        let list = core.finder.foundList
        let keys = list.map { "\($0.id)|\($0.name)" }
        if keys == lastFound && !foundBox.arrangedSubviews.isEmpty { return }
        lastFound = keys
        foundBox.arrangedSubviews.forEach { $0.removeFromSuperview() }
        if list.isEmpty { foundBox.addArrangedSubview(label("Looking for visible computers on this network…", secondary: true)); return }
        for f in list {
            let b = button("Link", self, #selector(linkTo(_:))); b.identifier = NSUserInterfaceItemIdentifier(f.id)
            let r = row([label(f.name), NSView(), b]); r.widthAnchor.constraint(equalToConstant: 480).isActive = true
            foundBox.addArrangedSubview(r)
        }
    }

    /// The code to compare, once per linking session.
    private func askCode(_ lv: Linker.View) {
        let key = "\(lv.asked)|\(lv.peerName)|\(lv.code)"
        guard sheetShownFor != key else { return }
        if let s = window.attachedSheet { window.endSheet(s) }
        sheetShownFor = key
        show()
        let a = NSAlert()
        let codeLabel = NSTextField(labelWithString: lv.code)
        codeLabel.font = .monospacedDigitSystemFont(ofSize: 34, weight: .semibold); codeLabel.alignment = .center
        codeLabel.frame = NSRect(x: 0, y: 0, width: 260, height: 44)
        a.accessoryView = codeLabel
        if lv.asked {
            a.messageText = "\(lv.peerName) wants to link with this Mac"
            a.informativeText = "Link only if \(lv.peerName) shows the same code. Its keyboard, mouse and clipboard will then work with this Mac."
            a.addButton(withTitle: "Link"); a.addButton(withTitle: "Don't Link")
        } else {
            a.messageText = "Does \(lv.peerName) show this code?"
            a.informativeText = "Check the code on \(lv.peerName), then confirm there too."
            a.addButton(withTitle: "Yes, Link"); a.addButton(withTitle: "Cancel")
        }
        a.beginSheetModal(for: window) { [weak self] r in
            guard let self, self.sheetShownFor == key else { return }
            self.core.linker.confirm(r == .alertFirstButtonReturn)
        }
    }

    func refreshTransfers() {
        let list = core.transfers.snapshot().prefix(5)
        let linked = core.locked { core.link != nil }
        transfersTitle.isHidden = list.isEmpty || !linked; transfersBox.isHidden = list.isEmpty || !linked; transfersLine.isHidden = transfersBox.isHidden
        transfersBox.arrangedSubviews.forEach { $0.removeFromSuperview() }
        for t in list {
            let title = label("\(t.direction) · \(t.what)", size: 12)
            var detail: String
            switch t.state {
            case .waiting: detail = "Starting…"
            case .running:
                detail = t.total > 0 ? "\(Int(t.fraction * 100))% · \(sizeText(t.done)) of \(sizeText(t.total))" : sizeText(t.done)
                if t.speed > 0 { detail += " · \(sizeText(UInt64(t.speed)))/s" }
            case .done: detail = "Done · \(sizeText(t.total))" + (t.total >= 1 << 20 && t.speed > 0 ? " at \(sizeText(UInt64(t.speed)))/s" : "")
            case .failed: detail = "Failed: \(t.reason)"
            }
            if t.pasteWaiting && (t.state == .waiting || t.state == .running) { detail += " · your paste will happen when this is done" }
            let d = label(detail, size: 11, secondary: true)
            if t.state == .failed { d.textColor = .systemRed }
            var views: [NSView] = [title]
            if t.state == .running || t.state == .waiting {
                let bar = NSProgressIndicator(); bar.isIndeterminate = t.state == .waiting || t.total == 0; bar.minValue = 0; bar.maxValue = 1
                bar.doubleValue = t.fraction; bar.style = .bar; bar.controlSize = .small
                bar.widthAnchor.constraint(equalToConstant: 480).isActive = true
                if bar.isIndeterminate { bar.startAnimation(nil) }
                views.append(bar)
            }
            views.append(d)
            let v = NSStackView(views: views); v.orientation = .vertical; v.alignment = .leading; v.spacing = 2
            transfersBox.addArrangedSubview(v)
        }
    }

    @objc private func changed(_ sender: Any) {
        let e: [Edge] = [.top, .bottom, .left, .right]
        let idx = side.indexOfSelectedItem
        core.update {
            $0.paused = sharing.state == .off
            $0.macDrivesPeer = macDrives.state == .on
            $0.peerDrivesMac = peerDrives.state == .on
            if idx >= 0 { $0.edge = e[idx] }
            $0.commandSuper = command.indexOfSelectedItem == 1
            $0.clipboard = clipboard.state == .on
            $0.usualShortcuts = usual.state == .on
            $0.bigFiles = bigFiles.state == .on
        }
        if (sender as? NSButton) === smooth { core.setSmooth(smooth.state == .on) }
        refresh()
    }
    func controlTextDidEndEditing(_ obj: Notification) {
        if let v = Int(limit.stringValue.trimmingCharacters(in: .whitespaces)), v > 0 { core.update { $0.fileLimitMB = v } }
        refresh()
    }
    @objc private func makeVisible() { core.finder.setVisible(true); refresh() }
    @objc private func stopVisibleNow() { core.finder.setVisible(false); refresh() }
    @objc private func linkTo(_ b: NSButton) {
        guard let id = b.identifier?.rawValue, let f = core.finder.foundList.first(where: { $0.id == id }) else { return }
        core.linker.link(to: f); refresh()
    }
    @objc private func cancelLinking() { core.linker.cancel(); refresh() }
    @objc private func forget() {
        let other = otherComputer()
        let a = NSAlert()
        a.messageText = "Forget \(other)?"
        a.informativeText = "Its keyboard, mouse and clipboard stop working with this Mac. To use them together again, link the two computers again."
        a.addButton(withTitle: "Forget"); a.addButton(withTitle: "Cancel")
        a.buttons[0].hasDestructiveAction = true
        a.beginSheetModal(for: window) { [weak self] r in
            guard r == .alertFirstButtonReturn, let self else { return }
            DispatchQueue.global().async { self.core.forget(tellPeer: true) }
        }
    }
    @objc private func setUpSmooth() {
        guard let script = Bundle.main.url(forResource: "install-smooth", withExtension: "sh") else { return }
        let cfg = NSWorkspace.OpenConfiguration()
        if let term = NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.apple.Terminal") {
            NSWorkspace.shared.open([script], withApplicationAt: term, configuration: cfg)
        }
    }
    @objc private func record(_ b: NSButton) {
        b.title = "Type the new shortcut…"
        if let r = recorder { NSEvent.removeMonitor(r) }
        recorder = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] ev in
            guard let self = self else { return ev }
            let f = UInt64(ev.modifierFlags.rawValue) & Shortcut.mask
            if ev.keyCode == 53 { self.stopRecording(); return nil }       // Escape: keep the old one
            guard f & (0x100000 | 0x40000 | 0x80000) != 0 else { NSSound.beep(); return nil }
            let sc = Shortcut(vk: ev.keyCode, flags: f)
            self.core.update { if b === self.copyKeyButton { $0.copyKey = sc } else { $0.pasteKey = sc } }
            self.stopRecording()
            return nil
        }
    }
    private func stopRecording() { if let r = recorder { NSEvent.removeMonitor(r) }; recorder = nil; refresh() }
    @objc func openAX() { NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")!) }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    var core: Core!
    var item: NSStatusItem!
    var win: MainWindow!
    var permTimer: Timer?, clipTimer: Timer?
    var showAtLaunch = true

    func applicationDidFinishLaunching(_ n: Notification) {
        if Installer.offerMove() { return }
        Installer.ejectInstallerDisk()
        let migrated = Migration.run()
        core = Core()
        buildMainMenu()
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        let menu = NSMenu(); menu.delegate = self; item.menu = menu
        win = MainWindow(core: core)
        let redraw = { [weak self] in self?.refreshIcon(); if self?.win.window.isVisible == true || self?.core.linker.locked({ self?.core.linker.view.stage == .confirm }) == true { self?.win.refresh() } }
        core.onChange = redraw
        core.finder.onChange = redraw
        core.linker.onChange = redraw
        core.finder.onRequestSocket = { [weak self] on in self?.core.linker.listen(on) }
        core.awdl.onChange = { [weak self] in self?.refreshIcon(); self?.win.refresh() }
        core.transfers.onChange = { [weak self] in self?.refreshIcon(); if self?.win.window.isVisible == true { self?.win.refreshTransfers() } }
        refreshIcon()
        NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main) { [weak self] _ in
            self?.core.refreshDisplays()
        }
        DistributedNotificationCenter.default().addObserver(forName: Notification.Name((Bundle.main.bundleIdentifier ?? "") + ".show"), object: nil, queue: .main) { [weak self] _ in
            self?.win.show()
        }
        core.start()
        if !inputDisabled {
            let opts = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
            if !AXIsProcessTrustedWithOptions(opts) { log("waiting for Accessibility permission") }
            tryTap()
            permTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in self?.tryTap() }
        }
        clipTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in self?.core.pollPasteboard() }
        Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in if self?.win.window.isVisible == true { self?.win.refresh() } }
        Migration.registerLoginItem()
        Updater.shared.start()
        if showAtLaunch || core.link == nil || migrated { win.show() }
        if migrated { Migration.explain(over: win.window) }
        log("started; \(core.link.map { "linked with \($0.peerName)" } ?? "not linked"), edge \(core.cfg.edge.rawValue), ports \(portBase)-\(discPort)")
    }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool { win.show(); return true }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let core else { return .terminateNow }
        core.prepareStop()
        core.stopDriving(sendLeave: false, pos: nil, why: "quit")   // the other computer keeps the pointer
        core.endDriven(why: "quit")
        core.awdl.restoreNow()
        return .terminateNow
    }

    func buildMainMenu() {
        let main = NSMenu(), appItem = NSMenuItem(), editItem = NSMenuItem(), windowItem = NSMenuItem()
        main.addItem(appItem); main.addItem(editItem); main.addItem(windowItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About Shenzhen KVM", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        addUpdaterMenuItems(to: appMenu)
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide Shenzhen KVM", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(withTitle: "Quit Shenzhen KVM", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        let edit = NSMenu(title: "Edit")
        for (t, s, k) in [("Undo", "undo:", "z"), ("Redo", "redo:", "Z"), ("Cut", "cut:", "x"), ("Copy", "copy:", "c"), ("Paste", "paste:", "v"), ("Select All", "selectAll:", "a")] {
            edit.addItem(withTitle: t, action: Selector(s), keyEquivalent: k)
        }
        editItem.submenu = edit
        let wm = NSMenu(title: "Window")
        wm.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        wm.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        windowItem.submenu = wm
        NSApp.mainMenu = main
    }

    func tryTap() {
        if core.tap != nil { return }
        if AXIsProcessTrusted() || CGPreflightListenEventAccess() {
            if core.installTap() { refreshIcon(); win.refresh() }
        }
    }

    func refreshIcon() {
        let (m, linked) = core.locked { (core.mode, core.link != nil) }
        let name: String
        switch m {
        case .driving: name = "keyboard.fill"
        case .driven: name = "keyboard.badge.ellipsis"
        default: name = (core.tap == nil && !inputDisabled) || !linked ? "exclamationmark.triangle" : "keyboard"
        }
        item.button?.image = NSImage(systemSymbolName: name, accessibilityDescription: "Shenzhen KVM")
        if let t = core.transfers.snapshot().first(where: { $0.state == .running && $0.total >= 1 << 20 }) {
            item.button?.title = " \(t.toPeer ? "↑" : "↓")\(Int(t.fraction * 100))%"
        } else { item.button?.title = "" }
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        let (m, online, paused, linked) = core.locked { (core.mode, core.peerOnline, core.cfg.paused, core.link != nil) }
        func info(_ s: String) { let i = NSMenuItem(title: s, action: nil, keyEquivalent: ""); i.isEnabled = false; menu.addItem(i) }
        func act(_ s: String, _ sel: Selector, _ on: Bool = false) {
            let i = NSMenuItem(title: s, action: sel, keyEquivalent: ""); i.target = self; i.state = on ? .on : .off; menu.addItem(i)
        }
        if core.tap == nil && !inputDisabled { info("Not working yet: needs permission (open the window)") }
        if !linked { info("Not linked yet") }
        else {
            switch m {
            case .driving: info("Mouse is on \(otherComputer())")
            case .driven: info("Mouse is on this Mac (\(otherComputer())'s mouse)")
            default: info(paused ? "Sharing is off" : online ? "Mouse is on this Mac · \(otherComputer()) connected" : "Mouse is on this Mac · \(otherComputer()) not answering")
            }
        }
        for t in core.transfers.snapshot().prefix(3) {
            let st: String
            switch t.state {
            case .waiting: st = "starting"
            case .running: st = t.total > 0 ? "\(Int(t.fraction * 100))% of \(sizeText(t.total))" : "sending"
            case .done: st = "done"
            case .failed: st = "failed"
            }
            info("\(t.direction): \(t.what) – \(st)\(t.pasteWaiting && t.state != .done && t.state != .failed ? ", paste waiting" : "")")
        }
        menu.addItem(.separator())
        act("Sharing", #selector(toggleSharing), !paused)
        if m == .driving { act("Bring the mouse back now", #selector(bringBack)) }
        act("Open Shenzhen KVM…", #selector(openWindow))
        menu.addItem(.separator())
        act("Quit Shenzhen KVM", #selector(quit))
    }
    @objc func toggleSharing() { core.setPaused(!core.locked { core.cfg.paused }) }
    @objc func bringBack() { core.stopDriving(sendLeave: true, pos: nil, why: "menu") }
    @objc func openWindow() { win.show() }
    @objc func quit() { NSApp.terminate(nil) }
}
