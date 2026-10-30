meta:
  id: session_protocol
  title: LambdaMOO session protocol 1.0
  endian: be
  encoding: UTF-8
doc: |
  The byte layout of the frames a session server and an edge exchange over
  the server's Unix socket. A carrier is a stream of frames; this type
  describes such a stream, or any sequence of whole frames taken from one.

  This file is normative for byte layouts. Session_Protocol.md is normative
  for everything else: which side may send which frame on which attachment,
  positions and acknowledgements, the rules a HELLO's kind and mode impose on
  its other fields, which connection fields are required, and what each
  frame means.
doc-ref: Session_Protocol.md
seq:
  - id: frames
    type: frame
    repeat: eos
types:
  frame:
    seq:
      - id: magic
        contents: MSES
      - id: version_major
        type: u1
        valid: 1
      - id: version_minor
        type: u1
        doc: A receiver accepts frames whose minor version is at most its own.
        valid:
          max: 0
      - id: frame_type
        type: u1
        enum: frame_type
        valid:
          any-of:
            - frame_type::hello
            - frame_type::welcome
            - frame_type::input
            - frame_type::output
            - frame_type::ack
            - frame_type::detach
            - frame_type::close
            - frame_type::error
            - frame_type::bind
            - frame_type::listeners
            - frame_type::connect
            - frame_type::connect_failed
            - frame_type::listen_failed
      - id: reserved
        type: u1
        valid: 0
      - id: flags
        type: u2
        doc: Reserved for capabilities negotiated in a future minor version.
        valid: 0
      - id: length
        type: u4
        doc: |
          The payload's length. INPUT and OUTPUT carry at most 1 MiB of data
          after their position; every other frame is at most 64 KiB.
        valid:
          max: 1048584
      - id: payload
        size: length
        type:
          switch-on: frame_type
          cases:
            'frame_type::hello': hello
            'frame_type::welcome': welcome
            'frame_type::input': data
            'frame_type::output': data
            'frame_type::ack': ack
            'frame_type::detach': detach
            'frame_type::close': diagnostic
            'frame_type::error': diagnostic
            'frame_type::bind': bind
            'frame_type::listeners': listeners
            'frame_type::connect': connect
            'frame_type::connect_failed': connect_failed
            'frame_type::listen_failed': listen_failed

  hello:
    doc: The first frame on every attachment, from the edge.
    seq:
      - id: session_id
        size: 16
      - id: edge_id
        size: 16
        doc: Fresh for each edge process.
      - id: generation
        type: u8
      - id: mode
        type: u1
        enum: mode
        valid:
          any-of: [mode::initial, mode::graceful, mode::crash]
      - id: kind
        type: u1
        enum: kind
        valid:
          any-of: [kind::client, kind::outbound, kind::control]
      - id: listen_port
        type: u2
        doc: The edge port a client arrived on, or 0 for the edge's own.
      - id: request
        type: u4
        doc: The CONNECT an outbound session answers, or 0.
      - id: input_position
        type: u8
      - id: output_position
        type: u8
      - id: player
        type: s8
        doc: The bound player, or INT64_MIN if not logged in.
      - id: listener
        type: s8
        doc: The bound listener, meaningful only with a player.
      - id: database
        size: 16
        doc: The digest from the session's last DETACH, or zeros.
      - id: fields_length
        type: u2
      - id: fields
        size: fields_length
        type: connection_fields

  connection_fields:
    doc: What the edge asserts about how the client reached it.
    seq:
      - id: fields
        type: connection_field
        repeat: eos
        doc: At most 32, each name at most once.

  connection_field:
    seq:
      - id: name_length
        type: u1
        valid:
          min: 1
          max: 32
      - id: name
        type: str
        size: name_length
        encoding: ASCII
        doc: Lowercase ASCII letters, digits and '-'.
      - id: value_type
        type: u1
        enum: field_type
        valid:
          any-of: [field_type::string, field_type::integer]
      - id: value
        type:
          switch-on: value_type
          cases:
            'field_type::string': field_string
            'field_type::integer': s8

  field_string:
    seq:
      - id: length
        type: u2
      - id: value
        type: str
        size: length
        encoding: ASCII
        doc: Printable ASCII, 0x20 to 0x7e.

  welcome:
    doc: The server's acceptance of a HELLO.
    seq:
      - id: generation
        type: u8
      - id: mode
        type: u1
        enum: mode
        valid:
          any-of: [mode::initial, mode::graceful, mode::crash]
      - id: reserved
        contents: [0, 0, 0, 0, 0, 0, 0]
      - id: input_position
        type: u8
      - id: output_position
        type: u8

  data:
    doc: INPUT from the edge, or OUTPUT from the server.
    seq:
      - id: position
        type: u8
        doc: The stream position of the first byte of data.
      - id: data
        size-eos: true

  ack:
    seq:
      - id: direction
        type: u1
        enum: direction
        valid:
          any-of: [direction::input, direction::output]
      - id: reserved
        contents: [0, 0, 0, 0, 0, 0, 0]
      - id: position
        type: u8
        doc: The next position accepted.

  detach:
    seq:
      - id: mode
        type: u1
        enum: mode
        valid:
          any-of:
            - mode::graceful
            - mode::crash
      - id: reserved
        contents: [0, 0, 0, 0, 0, 0, 0]
      - id: input_position
        type: u8
      - id: output_position
        type: u8
      - id: database
        size: 16
        doc: MD5 digest of the database file just written.

  diagnostic:
    doc: CLOSE or ERROR, with an optional diagnostic that isn't trusted data.
    seq:
      - id: text
        type: str
        size-eos: true

  bind:
    seq:
      - id: player
        type: s8
      - id: listener
        type: s8

  listeners:
    doc: The complete set of ports the server listens on through the edge.
    seq:
      - id: serial
        type: u4
        valid:
          min: 1
        doc: Increases with each LISTENERS the server sends.
      - id: entries
        type: listen_entry
        repeat: eos

  listen_entry:
    seq:
      - id: port
        type: u2
        valid:
          min: 1
      - id: flags
        type: u2
        valid:
          max: 3
      - id: certificate_length
        type: u2
      - id: key_length
        type: u2
      - id: acme_length
        type: u2
      - id: certificate
        type: str
        size: certificate_length
      - id: key
        type: str
        size: key_length
      - id: acme
        type: str
        size: acme_length
        doc: Comma-separated host names.
    instances:
      tls:
        value: (flags & 1) != 0
      acme_requested:
        value: (flags & 2) != 0

  connect:
    doc: The server asking the edge for an outbound connection.
    seq:
      - id: request
        type: u4
        valid:
          min: 1
      - id: port
        type: u2
      - id: flags
        type: u2
        valid:
          max: 1
      - id: timeout
        type: u4
        doc: Seconds, or 0 for the edge's default.
      - id: host_length
        type: u2
        valid:
          min: 1
      - id: reserved
        contents: [0, 0]
      - id: host
        type: str
        size: host_length
    instances:
      tls:
        value: (flags & 1) != 0

  connect_failed:
    seq:
      - id: request
        type: u4
        valid:
          min: 1
      - id: reason
        type: u1
        enum: connect_failure
        valid:
          any-of: [connect_failure::destination, connect_failure::resources]
      - id: reserved
        contents: [0, 0, 0]
      - id: diagnostic
        type: str
        size-eos: true

  listen_failed:
    seq:
      - id: port
        type: u2
        valid:
          min: 1
      - id: reason
        type: u1
        enum: listen_failure
        valid:
          any-of: [listen_failure::port, listen_failure::certificate]
      - id: reserved
        contents: [0]
      - id: serial
        type: u4
        valid:
          min: 1
        doc: The serial of the LISTENERS that named the port.
      - id: diagnostic
        type: str
        size-eos: true

enums:
  frame_type:
    1: hello
    2: welcome
    3: input
    4: output
    5: ack
    6: detach
    7: close
    8: error
    9: bind
    10: listeners
    11: connect
    12: connect_failed
    13: listen_failed
  mode:
    1: initial
    2: graceful
    3: crash
  kind:
    0: client
    1: outbound
    2: control
  direction:
    1: input
    2: output
  field_type:
    1: string
    2: integer
  connect_failure:
    1: destination
    2: resources
  listen_failure:
    1: port
    2: certificate
