.. _mikroe_ad_swio_3_click_shield:

MikroElektronika AD-SWIO 3 Click
################################

Overview
********

The MikroElektronika AD-SWIO 3 Click carries the Analog Devices `AD74115H`_
single-channel software configurable input/output in a `mikroBUS`_ form
factor.

The shield configures the channel for voltage input and exposes the primary
ADC conversion as ADC channel 0, measuring 0 V to 12 V with 16-bit resolution.

Requirements
************

This shield can only be used with a development board that provides a
configuration for mikroBUS connectors and defines node labels for the mikroBUS
SPI interface and the mikroBUS header (see :ref:`shields` for more details).

For more information about the AD74115H and the AD-SWIO 3 Click, see the
following documentation:

- `AD74115H`_
- `AD-SWIO 3 Click`_

Programming
***********

Set ``--shield mikroe_ad_swio_3_click`` when you invoke ``west build``. For
example:

.. zephyr-app-commands::
   :app: <my_app>
   :board: balletto_b1_dk/ab1c1f4m51820ph0
   :shield: mikroe_ad_swio_3_click
   :goals: build

.. _AD74115H:
   https://www.analog.com/en/products/ad74115h.html

.. _mikroBUS:
   https://www.mikroe.com/mikrobus

.. _AD-SWIO 3 Click:
   https://www.mikroe.com/ad-swio-3-click
