{
  'target_defaults': {
    'variables': {
      'deps': [
        'libbrillo-<(libbase_ver)',
        'libchrome-<(libbase_ver)',
        'system_api',
      ],
    },
    'include_dirs': [
      '.',
    ],
  },
  'targets': [
    {
      'target_name': 'buffet_common',
      'type': 'static_library',
      'variables': {
        'dbus_adaptors_out_dir': 'include/buffet',
        'dbus_service_config': 'dbus_bindings/dbus-service-config.json',
        'exported_deps': [
          'libshill-client',
          'libweave-<(libbase_ver)',
        ],
        'deps': ['>@(exported_deps)'],
      },
      'all_dependent_settings': {
        'variables': {
          'deps': [
            '<@(exported_deps)',
          ],
        },
      },
      'sources': [
        'buffet_config.cc',
        'dbus_bindings/org.chromium.Buffet.Command.xml',
        'dbus_bindings/org.chromium.Buffet.Manager.xml',
        'dbus_command_dispatcher.cc',
        'dbus_command_proxy.cc',
        'dbus_conversion.cc',
        'dbus_constants.cc',
        'http_transport_client.cc',
        'manager.cc',
        'shill_client.cc',
        'socket_stream.cc',
      ],
      'conditions': [
        ['USE_wifi_bootstrapping == 1', {
          'variables': {
            'exported_deps': [
              'libapmanager-client',
              'libpeerd-client',
              'libwebserv-<(libbase_ver)',
            ],
          },
          'all_dependent_settings': {
            'defines': [ 'BUFFET_USE_WIFI_BOOTSTRAPPING' ],
          },
          'defines': [ 'BUFFET_USE_WIFI_BOOTSTRAPPING' ],
          'sources': [
            'ap_manager_client.cc',
            'peerd_client.cc',
            'webserv_client.cc',
          ],
        }],
      ],
      'includes': ['../common-mk/generate-dbus-adaptors.gypi'],
      'actions': [
        {
          'action_name': 'generate-buffet-proxies',
          'variables': {
            'dbus_service_config': 'dbus_bindings/dbus-service-config.json',
            'proxy_output_file': 'include/buffet/dbus-proxies.h',
          },
          'sources': [
            'dbus_bindings/org.chromium.Buffet.Command.xml',
            'dbus_bindings/org.chromium.Buffet.Manager.xml',
          ],
          'includes': ['../common-mk/generate-dbus-proxies.gypi'],
        },
      ],
    },
    {
      'target_name': 'buffet',
      'type': 'executable',
      'dependencies': [
        'buffet_common',
      ],
      'sources': [
        'main.cc',
      ],
    },
    {
      'target_name': 'buffet_client',
      'type': 'executable',
      'sources': [
        'buffet_client.cc',
      ],
    },
  ],
  'conditions': [
    ['USE_test == 1', {
      'targets': [
        {
          'target_name': 'buffet_testrunner',
          'type': 'executable',
          'dependencies': [
            'buffet_common',
          ],
          'variables': {
            'deps': [
              'libbrillo-test-<(libbase_ver)',
              'libchrome-test-<(libbase_ver)',
              'libweave-test-<(libbase_ver)',
            ],
          },
          'includes': ['../common-mk/common_test.gypi'],
          'sources': [
            'buffet_config_unittest.cc',
            'buffet_testrunner.cc',
            'dbus_command_proxy_unittest.cc',
            'dbus_conversion_unittest.cc',
          ],
        },
      ],
    }],
  ],
}
