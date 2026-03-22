################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../sensor/sensor_hal.c \
../sensor/sensors.c 

OBJS += \
./sensor/sensor_hal.o \
./sensor/sensors.o 

C_DEPS += \
./sensor/sensor_hal.d \
./sensor/sensors.d 


# Each subdirectory must supply rules for building sources it contributes
sensor/%.o sensor/%.su sensor/%.cyclo: ../sensor/%.c sensor/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m3 -std=gnu11 -DUSE_HAL_DRIVER -DSTM32F103xB -c -I../Core/Inc -I../Drivers/STM32F1xx_HAL_Driver/Inc -I../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F1xx/Include -I../Drivers/CMSIS/Include -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-sensor

clean-sensor:
	-$(RM) ./sensor/sensor_hal.cyclo ./sensor/sensor_hal.d ./sensor/sensor_hal.o ./sensor/sensor_hal.su ./sensor/sensors.cyclo ./sensor/sensors.d ./sensor/sensors.o ./sensor/sensors.su

.PHONY: clean-sensor

