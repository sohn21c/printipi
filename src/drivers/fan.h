/* The MIT License (MIT)
 *
 * Copyright (c) 2014 Colin Wallace
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* 
 * Printipi/drivers/fan.h
 *
 * The Fan class serves to control a physical Fan, often used to cool cpu components.
 * This class essentially wraps an IoPin so that it can be commanded and recognized as a fan.
 */
 

#ifndef DRIVERS_FAN_H
#define DRIVERS_FAN_H
 
#include <utility> //for std::move
#include "iodriver.h"
#include "drivers/iopin.h"

namespace drv {

class Fan : public IODriver {
    IoPin pin;
    public:
        inline Fan(IoPin &&pin) : IODriver(), pin(std::move(pin)) {
            this->pin.makeDigitalOutput(IoLow);
        }
        inline bool isFan() const { return true; }
        inline const IoPin& getPwmPin() {
            return pin;
        }
};

}
#endif
