// Copyright 2026 Proify, Tomakino. SPDX-License-Identifier: Apache-2.0
package io.github.proify.lyricon.provider;

import android.content.Intent;
import android.os.Bundle;

interface IProviderService {
    Bundle onRunCommand(in Intent intent);
}
