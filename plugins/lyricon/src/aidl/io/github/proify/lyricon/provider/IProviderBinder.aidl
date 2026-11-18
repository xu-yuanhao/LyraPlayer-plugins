// Copyright 2026 Proify, Tomakino. SPDX-License-Identifier: Apache-2.0
package io.github.proify.lyricon.provider;

import io.github.proify.lyricon.provider.IRemoteService;
import io.github.proify.lyricon.provider.IProviderService;

interface IProviderBinder {
    void onRegistrationCallback(IRemoteService service);
    IProviderService getProviderService();
    byte[] getProviderInfo();
}
