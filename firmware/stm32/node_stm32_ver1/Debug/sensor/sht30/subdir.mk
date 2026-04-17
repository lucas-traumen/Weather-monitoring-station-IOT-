################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../sensor/sht30/driver_sht30.c 

OBJS += \
./sensor/sht30/driver_sht30.o 

C_DEPS += \
./sensor/sht30/driver_sht30.d 


# Each subdirectory must supply rules for building sources it contributes
sensor/sht30/%.o sensor/sht30/%.su sensor/sht30/%.cyclo: ../sensor/sht30/%.c sensor/sht30/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m3 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F103xB -c -I../Core/Inc -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/sensor" -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/sensor" -I"D:/stm32workspacecubeide/bulepill/Driver/CMSIS/DSP/Include" -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/sensor/bmp388" -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/sensor/sht30" -I../Drivers/STM32F1xx_HAL_Driver/Inc -I../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F1xx/Include -I../Drivers/CMSIS/Include -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/sensor" -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -fshort-enums -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-sensor-2f-sht30

clean-sensor-2f-sht30:
	-$(RM) ./sensor/sht30/driver_sht30.cyclo ./sensor/sht30/driver_sht30.d ./sensor/sht30/driver_sht30.o ./sensor/sht30/driver_sht30.su

.PHONY: clean-sensor-2f-sht30

