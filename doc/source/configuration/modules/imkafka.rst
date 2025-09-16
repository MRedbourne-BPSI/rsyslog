*******************************
imkafka: read from Apache Kafka
*******************************

===========================  ===========================================================================
**Module Name:**             **imkafka**
**Author:**                  Andre Lorbach <alorbach@adiscon.com>
**Available since:**         8.27.0
===========================  ===========================================================================


Purpose
=======

The imkafka plug-in implements an Apache Kafka consumer, permitting
rsyslog to receive data from Kafka.


Configuration Parameters
========================

Note that imkafka supports some *Array*-type parameters. While the parameter
name can only be set once, it is possible to set multiple values with that
single parameter.

For example, to select a broker, you can use

.. code-block:: none

   input(type="imkafka" topic="mytopic" broker="localhost:9092" consumergroup="default")

which is equivalent to

.. code-block:: none

   input(type="imkafka" topic="mytopic" broker=["localhost:9092"] consumergroup="default")

To specify multiple values, just use the bracket notation and create a
comma-delimited list of values as shown here:

.. code-block:: none

   input(type="imkafka" topic="mytopic"
          broker=["localhost:9092",
                  "localhost:9093",
                  "localhost:9094"]
         )


.. note::

   Parameter names are case-insensitive.


Module Parameters
-----------------

Currently none.


Action Parameters
-----------------

Broker
^^^^^^

.. csv-table::
   :header: "type", "default", "mandatory", "|FmtObsoleteName| directive"
   :widths: auto
   :class: parameter-table

   "array", "localhost:9092", "no", "none"

Specifies the broker(s) to use.


Topic
^^^^^

.. csv-table::
   :header: "type", "default", "mandatory", "|FmtObsoleteName| directive"
   :widths: auto
   :class: parameter-table

   "string", "none", "yes", "none"

Specifies the topic to produce to.


ConfParam
^^^^^^^^^

.. csv-table::
   :header: "type", "default", "mandatory", "|FmtObsoleteName| directive"
   :widths: auto
   :class: parameter-table

   "array", "none", "no", "none"

Permits to specify Kafka options. Rather than offering a myriad of
config settings to match the Kafka parameters, we provide this setting
here as a vehicle to set any Kafka parameter. This has the big advantage
that Kafka parameters that come up in new releases can immediately be used.

Note that we use librdkafka for the Kafka connection, so the parameters
are actually those that librdkafka supports. As of our understanding, this
is a superset of the native Kafka parameters.


ConsumerGroup
^^^^^^^^^^^^^

.. csv-table::
   :header: "type", "default", "mandatory", "|FmtObsoleteName| directive"
   :widths: auto
   :class: parameter-table

   "string", "none", "no", "none"

With this parameter the group.id for the consumer is set. All consumers
sharing the same group.id belong to the same group.


Ruleset
^^^^^^^

.. csv-table::
   :header: "type", "default", "mandatory", "|FmtObsoleteName| directive"
   :widths: auto
   :class: parameter-table

   "string", "none", "no", "none"

Specifies the ruleset to be used.


ParseHostname
^^^^^^^^^^^^^

.. csv-table::
   :header: "type", "default", "mandatory", "|FmtObsoleteName| directive"
   :widths: auto
   :class: parameter-table

   "binary", "off", "no", "none"

.. versionadded:: 8.38.0

If this parameter is set to on, imkafka will parse the hostname in log
if it exists. The result can be retrieved from $hostname. If it's off,
for compatibility reasons, the local hostname is used, same as the previous
version.

===========================
imkafka Statistics Counters
===========================

Counters
========

Global Counters
---------------
These counters appear under the global stats object ``name="imkafka"``:

=========================  ======================  ============================================
Counter Name              Unit                   Description
=========================  ======================  ============================================
``received``              messages               Total messages successfully polled from Kafka
``submitted``             messages               Messages successfully submitted to rsyslog core
``failures``              messages               Errors during poll or submission
``eof``                   events                 Number of partition EOF events encountered
``poll_empty``            polls                  Number of consumer polls returning no message
``maxlag``                messages               Maximum observed consumer lag (backlog size)
``rtt_avg_usec``          microseconds           Average broker round-trip time (from librdkafka)
``throttle_avg_msec``     milliseconds           Average broker throttle time (from librdkafka)
``int_latency_avg_usec``  microseconds           Average internal librdkafka latency
``errors_timed_out``      count                  Kafka timeouts reported via error callback
``errors_transport``      count                  Transport-level errors
``errors_broker_down``    count                  All brokers down errors
``errors_auth``           count                  Authentication failures
``errors_ssl``            count                  SSL/TLS errors
``errors_other``          count                  Other librdkafka errors
=========================  ======================  ============================================

Per-Instance Counters
----------------------
Each imkafka input instance also exposes its own stats object named:

.. code-block:: none

    imkafka[<topic>|<consumergroup>]

Per-instance counters include:

===================  ======================  ============================================
Counter Name        Unit                   Description
===================  ======================  ============================================
``received``        messages               Messages polled from Kafka for this instance
``submitted``       messages               Messages submitted to rsyslog core
``failures``        messages               Errors during poll or submission
``eof``             events                 Partition EOF events for this instance
``poll_empty``      polls                  Empty poll cycles for this instance
``maxlag``          messages               Maximum observed lag for this instance
===================  ======================  ============================================

Units and Semantics
====================
* **maxlag**: Represents the maximum observed backlog in **number of messages**, computed as:

  .. code-block:: none

      high_watermark_offset - current_offset - 1

  This is **not** a time measurement.

* **rtt_avg_usec**, **throttle_avg_msec**, **int_latency_avg_usec**: Derived from librdkafka's JSON statistics. These are time-based metrics:
  
  - ``rtt_avg_usec``: Average broker round-trip time in microseconds.
  - ``throttle_avg_msec``: Average broker throttle time in milliseconds.
  - ``int_latency_avg_usec``: Average internal librdkafka latency in microseconds.

Enabling Librdkafka Stats
==========================
To populate the window metrics (``rtt_avg_usec``, ``throttle_avg_msec``, ``int_latency_avg_usec``), set:

.. code-block:: none

    confparam=["statistics.interval.ms=10000"]

This instructs librdkafka to emit JSON stats every 10 seconds, which imkafka parses and exposes via impstats.

Example impstats Output (format=legacy)
========================
.. code-block:: none

    name="imkafka" origin="imkafka" received=12345 submitted=12340 failures=5 eof=10 poll_empty=200 maxlag=150 \
    rtt_avg_usec=500 throttle_avg_msec=0 int_latency_avg_usec=120 \
    errors_timed_out=0 errors_transport=0 errors_broker_down=0 errors_auth=0 errors_ssl=0 errors_other=0

    name="imkafka[logs|rsyslog-cg]" origin="imkafka" received=12345 submitted=12340 failures=5 eof=10 poll_empty=200 maxlag=150

Notes
=====
* ``log.file`` output from impstats is **not** subject to ``$MaxMessageSize``.
* ``log.syslog`` output **is** subject to ``$MaxMessageSize``. If you opt to use ``format="json-array"`` **and** ``log.syslog="on"``  option in your impstats configuration, be mindful of your pstat message size and truncation. Truncation in json-array will likely break monitoring metrics for systems reliant on JSON parsing such as Nagios and Zabbix. You may need to specify $MaxMessageSize at the top of /etc/rsyslog.conf to accomodate large stats outputs.
* ``statistics.interval.ms=<Duration>`` is an optional configuration if you want to populate latency variables.

.. versionadded:: ??.??.??


Caveats/Known Bugs
==================

-  currently none


Examples
========

Example 1
---------

In this sample a consumer for the topic static is created and will forward the messages to the omfile action.

.. code-block:: none

   module(load="imkafka")
   input(type="imkafka" topic="static" broker="localhost:9092"
                        consumergroup="default" ruleset="pRuleset")

   ruleset(name="pRuleset") {
   	action(type="omfile" file="path/to/file")
   }
