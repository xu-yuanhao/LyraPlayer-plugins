// Copyright 2026 Proify, Tomakino. SPDX-License-Identifier: Apache-2.0
package io.github.proify.lyricon.provider;

import io.github.proify.lyricon.provider.IRemotePlayer;

interface IRemoteService {
    IRemotePlayer getPlayer();
    void disconnect();
}
