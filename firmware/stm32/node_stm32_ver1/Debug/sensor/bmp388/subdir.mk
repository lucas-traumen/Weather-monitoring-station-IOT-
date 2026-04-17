################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../sensor/bmp388/driver_bmp388.c 

OBJS += \
./sensor/bmp388/driver_bmp388.o 

C_DEPS += \
./sensor/bmp388/driver_bmp388.d 


# Each subdirectory must supply rules for building sources it contributes
sensor/bmp388/%.o sensor/bmp388/%.su sensor/bmp388/%.cyclo: ../sensor/bmp388/%.c sensor/bmp388/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m3 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F103xB -c -I../Core/Inc -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/sensor" -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/sensor" -I"D:/stm32workspacecubeide/bulepill/Driver/CMSIS/DSP/Include" -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/sensor/bmp388" -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/sensor/sht30" -I../Drivers/STM32F1xx_HAL_Driver/Inc -I../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F1xx/Include -I../Drivers/CMSIS/Include -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/sensor" -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -fshort-enums -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-sensor-2f-bmp388

clean-sensor-2f-bmp388:
	-$(RM) ./sensor/bmp388/driver_bmp388.cyclo ./sensor/bmp388/driver_bmp388.d ./sensor/bmp388/driver_bmp388.o ./sensor/bmp388/driver_bmp388.su

.PHONY: clean-sensor-2f-bmp388

