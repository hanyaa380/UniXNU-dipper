// SPDX-License-Identifier: MIT
// Qcom adaptation of AdrenoMetal 88849ec NativeOsLogExport.mm's writePass.
// Keep one native enumerator across bounded pages; a final footer proves EOF.
void writePass(unsigned pass) {
    @autoreleasepool {
        gzFile activeOut = nullptr;
        @try {
            const std::string directory = kDirectory;
            const std::string temporary = directory + "/.oslog.txt.gz", final = directory + "/oslog.txt.gz";
            unsigned page = 1;
            uint64_t lines = 0, pageLines = 0, first = 1;
            bool failed = false;
            gzFile out = nullptr;
            auto beginPage = [&]() {
                out = gzopen(temporary.c_str(), "wb6"); activeOut = out;
                if (!out) { QcomUserLog::status("gzip_open_failed"); return false; }
                if (gzprintf(out, "# qcom oslog pass %u page=%u first_entry=%llu scope=system since_boot=1 max_lines_per_page=60000 max_text_bytes_per_page=8388608 max_pages=8\n",
                             pass, page, (unsigned long long)first) <= 0) {
                    QcomUserLog::status("gzip_header_write_failed"); return false;
                }
                return true;
            };
            auto finishPage = [&](bool last) {
                bool written = gzprintf(out, "# %llu entries\n# qcom_page pass=%u page=%u first=%llu count=%llu final=%u total=%llu\n",
                                        (unsigned long long)pageLines, pass, page, (unsigned long long)first,
                                        (unsigned long long)pageLines, last ? 1u : 0u, (unsigned long long)lines) > 0;
                int closed = gzclose(out); out = activeOut = nullptr;
                if (!written || closed != Z_OK) { QcomUserLog::status("gzip_close_or_write_failed"); return false; }
                if (chmod(temporary.c_str(), 0644) || rename(temporary.c_str(), final.c_str())) {
                    QcomUserLog::status("snapshot_rename_or_mode_failed"); return false;
                }
                return QcomUserLog::snapshot(final.c_str(), pass);
            };
            if (!beginPage()) { if (out) gzclose(out); activeOut = nullptr; return; }
            static void *framework = nullptr;
            if (!framework) {
                dlerror();
                framework = dlopen("/System/Library/Frameworks/OSLog.framework/OSLog", RTLD_NOW | RTLD_LOCAL);
                if (!framework) {
                    const char *reason = dlerror();
                    gzprintf(out, "# framework load failed: %.1024s\n", reason ?: "no dlerror");
                    QcomUserLog::status("oslog_framework_load_failed");
                }
            }
            Class storeClass = NSClassFromString(@"OSLogStore");
            char progress[160];
            snprintf(progress, sizeof progress, "oslog_class found=%u framework=%u", storeClass != Nil, framework != nullptr);
            QcomUserLog::status(progress);
            NSError *error = nil;
            id store = storeClass ? ((id (*)(Class, SEL, NSInteger, NSError **))objc_msgSend)(storeClass, @selector(storeWithScope:error:), 0, &error) : nil;
            char osbuild[32] = {};
            size_t buildLength = sizeof osbuild;
            const bool pinnedBuild = sysctlbyname("kern.osversion", osbuild, &buildLength, nullptr, 0) == 0 &&
                buildLength > 0 && buildLength <= sizeof osbuild && osbuild[buildLength-1] == 0 &&
                strcmp(osbuild, "20H392") == 0;
            if (!store && !error && storeClass && pinnedBuild) {
                store = QcomUserLog::openIos16SystemStore(storeClass, &error);
                QcomUserLog::status(store ? "ios16_system_store prepared=1" : "ios16_system_store prepared=0");
            }
            if (!store) {
                gzprintf(out, "# no store: %s\n", (error ? error.description : (storeClass ? @"OSLogStore factory returned nil without NSError" : @"OSLogStore missing")).UTF8String);
            } else {
                snprintf(progress, sizeof progress, "oslog_enumeration_begin pass=%u", pass);
                QcomUserLog::status(progress);
                id position = ((id (*)(id, SEL, NSTimeInterval))objc_msgSend)(store, @selector(positionWithTimeIntervalSinceLatestBoot:), 0);
                id entries = ((id (*)(id, SEL, NSUInteger, id, id, NSError **))objc_msgSend)(store, @selector(entriesEnumeratorWithOptions:position:predicate:error:), 0, position, nil, &error);
                QcomUserLog::status(entries ? "oslog_enumerator ready=1" : "oslog_enumerator ready=0");
                if (!entries) gzprintf(out, "# no entries: %s\n", (error ? error.description : @"nil").UTF8String);
                NSDate *start = nil;
                // Fast enumeration fetches its batches before entering the
                // loop body's pool. Fetch inside it too, so native temporary
                // objects do not accumulate in the pass-wide pool.
                for (;;) {
                    @autoreleasepool {
                        id entry = ((id (*)(id, SEL))objc_msgSend)(entries, @selector(nextObject));
                        if (!entry) break;
                        NSDate *date = [entry valueForKey:@"date"];
                        if (!start) start = date;
                        const double at = [date timeIntervalSinceDate:start];
                        NSString *message = [entry valueForKey:@"composedMessage"];
                        const bool isLog = [entry respondsToSelector:@selector(level)];
                        NSString *process = [entry respondsToSelector:@selector(process)] ? [entry valueForKey:@"process"] : @"?";
                        NSString *subsystem = [entry respondsToSelector:@selector(subsystem)] ? [entry valueForKey:@"subsystem"] : @"";
                        NSString *category = [entry respondsToSelector:@selector(category)] ? [entry valueForKey:@"category"] : @"";
                        const int pid = [entry respondsToSelector:@selector(processIdentifier)] ? [[entry valueForKey:@"processIdentifier"] intValue] : 0;
                        const NSInteger level = isLog ? [[entry valueForKey:@"level"] integerValue] : 0;
                        const char *format = "%9.3f %5d %s %s:%s %s %s\n";
                        const char *proc = process.UTF8String ?: "?", *sub = subsystem.UTF8String ?: "";
                        const char *cat = category.UTF8String ?: "", *msg = message.UTF8String ?: "";
                        int length = snprintf(nullptr, 0, format, at, pid, proc, sub, cat, levelName(level), msg);
                        if (length <= 0 || length > 65536) {
                            gzprintf(out, "# TRUNCATED oversized_or_invalid_entry\n"); failed = true; break;
                        }
                        if (pageLines && (pageLines >= kLineLimit || gztell(out) + length + 512 > 8388608)) {
                            if (page == 8) { gzprintf(out, "# TRUNCATED page_limit\n"); failed = true; break; }
                            if (!finishPage(false)) { failed = true; break; }
                            ++page; first = lines + 1; pageLines = 0;
                            if (!beginPage()) { failed = true; break; }
                        }
                        std::string text((size_t)length + 1, '\0');
                        snprintf(&text[0], text.size(), format, at, pid, proc, sub, cat, levelName(level), msg);
                        if (gzwrite(out, text.data(), length) != length) { failed = true; break; }
                        ++lines; ++pageLines;
                        if (lines == 1 || lines % 4096 == 0) {
                            snprintf(progress, sizeof progress, "oslog_enumeration_progress pass=%u entries=%llu", pass, (unsigned long long)lines);
                            QcomUserLog::status(progress);
                        }
                    }
                }
                snprintf(progress, sizeof progress, "oslog_enumeration_end pass=%u entries=%llu", pass, (unsigned long long)lines);
                QcomUserLog::status(progress);
            }
            if (out) finishPage(!failed);
        } @catch (NSException *exception) {
            if (activeOut) gzclose(activeOut);
            char message[512];
            snprintf(message, sizeof message, "oslog_exception name=%.120s reason=%.280s", exception.name.UTF8String ?: "?", exception.reason.UTF8String ?: "?");
            QcomUserLog::status(message);
        }
    }
}
