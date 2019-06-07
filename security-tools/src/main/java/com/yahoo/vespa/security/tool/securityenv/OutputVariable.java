// Copyright 2019 Oath Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package com.yahoo.vespa.security.tool.securityenv;

/**
 * Define the possible environment variables that the program may output.
 *
 * @author bjorncs
 */
enum OutputVariable {
    TLS_ENABLED("VESPA_TLS_ENABLED", "Set to '1' if TLS is enabled in Vespa"),
    CA_CERTIFICATE("VESPA_TLS_CA_CERT", "Path to CA certificates file"),
    CERTIFICATE("VESPA_TLS_CERT", "Path to certificate file"),
    PRIVATE_KEY("VESPA_TLS_PRIVATE_KEY", "Path to private key file");

    private final String variableName;
    private final String description;

    OutputVariable(String variableName, String description) {
        this.variableName = variableName;
        this.description = description;
    }

    String variableName() {
        return variableName;
    }

    String description() {
        return description;
    }
}