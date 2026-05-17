################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
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
	arm-none-eabi-gcc "$<" -mcpu=cortex-m3 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F103xB -c -I../Core/Inc -I"/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/stm32/node_stm32_ver1/sensor" -I"/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/stm32/node_stm32_ver1/sensor" -I"D:/stm32workspacecubeide/bulepill/Driver/CMSIS/DSP/Include" -I"/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/stm32/node_stm32_ver1/sensor/bmp388" -I"/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/stm32/node_stm32_ver1/sensor/sht30" -I../Drivers/STM32F1xx_HAL_Driver/Inc -I../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F1xx/Include -I../Drivers/CMSIS/Include -I"/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/stm32/node_stm32_ver1/sensor" -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -fshort-enums -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-sensor

clean-sensor:
	-$(RM) ./sensor/sensor_hal.cyclo ./sensor/sensor_hal.d ./sensor/sensor_hal.o ./sensor/sensor_hal.su ./sensor/sensors.cyclo ./sensor/sensors.d ./sensor/sensors.o ./sensor/sensors.su

.PHONY: clean-sensor

