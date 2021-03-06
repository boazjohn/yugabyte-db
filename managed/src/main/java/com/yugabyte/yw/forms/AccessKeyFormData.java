// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.forms;

import com.yugabyte.yw.common.AccessManager;
import play.data.validation.Constraints;

import java.util.UUID;

public class AccessKeyFormData {
    @Constraints.Required()
    public String keyCode;

    @Constraints.Required()
    public UUID regionUUID;

    public AccessManager.KeyType keyType;

    public String keyContent;

    public String sshUser;

    // Normally cloud access keys are created on provider creation, so default to 22 for onprem
    // providers.
    public Integer sshPort = 22;

    public boolean passwordlessSudoAccess = true;

    public boolean airGapInstall = false;
}
