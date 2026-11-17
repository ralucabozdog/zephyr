.. _mikroe_iso_adc_2_click_shield:

MikroElektronika ISO ADC 2 Click
################################

Overview
********

The MikroElektronika ISO ADC 2 Click carries the Analog Devices `AD7091R`_
12-bit, 1 MSPS successive approximation ADC in a `mikroBUS`_ form factor. The
`ADuM5401`_ isolates the SPI signals and supplies the isolated side of the
board.

The shield exposes the ADC input as ADC channel 0, measuring 0 V to 2.5 V
with 12-bit resolution against the AD7091R internal reference. Conversions
are started on the CONVST pin, which the board routes to the mikroBUS RST
pin.

Requirements
************

This shield can only be used with a development board that provides a
configuration for mikroBUS connectors and defines node labels for the mikroBUS
SPI interface and the mikroBUS header (see :ref:`shields` for more details).

For more information about the AD7091R and the ISO ADC 2 Click, see the
following documentation:

- `AD7091R`_
- `ISO ADC 2 Click`_

Programming
***********

Set ``--shield mikroe_iso_adc_2_click`` when you invoke ``west build``. For
example:

.. zephyr-app-commands::
   :app: <my_app>
   :board: balletto_b1_dk/ab1c1f4m51820ph0
   :shield: mikroe_iso_adc_2_click
   :goals: build

.. _AD7091R:
   https://www.analog.com/en/products/ad7091r.html

.. _ADuM5401:
   https://www.analog.com/en/products/adum5401.html

.. _mikroBUS:
   https://www.mikroe.com/mikrobus

.. _ISO ADC 2 Click:
   https://www.mikroe.com/iso-adc-2-click
