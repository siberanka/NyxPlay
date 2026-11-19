package com.limelight.utils;

import java.util.concurrent.Executor;

/** Keeps tests that resume the host list from reaching the network for an update check. */
public final class NoUpdateCheck {
    private NoUpdateCheck() {}

    public static void install() {
        UpdateNotifier.setTestHooks(new UpdateNotifier.Fetcher() {
            @Override
            public UpdateChecker.Result fetch() {
                return UpdateChecker.Result.upToDate("0");
            }
        }, new Executor() {
            @Override
            public void execute(Runnable command) {
                command.run();
            }
        });
    }

    public static void remove() {
        UpdateNotifier.setTestHooks(null, null);
    }
}
