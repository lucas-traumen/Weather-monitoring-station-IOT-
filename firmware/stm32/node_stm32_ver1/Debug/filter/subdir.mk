################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../filter/ema.c \
../filter/kalman.c 

OBJS += \
./filter/ema.o \
./filter/kalman.o 

C_DEPS += \
./filter/ema.d \
./filter/kalman.d 


# Each subdirectory must supply rules for building sources it contributes
filter/%.o filter/%.su filter/%.cyclo: ../filter/%.c filter/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m3 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F103xB -c -I../Core/Inc -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/sensor" -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/sensor/bmp388" -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/sensor/sht30" -I"D:/stm32workspacecubeide/bulepill/Driver/CMSIS/DSP/Include" -I../Drivers/STM32F1xx_HAL_Driver/Inc -I../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F1xx/Include -I../Drivers/CMSIS/Include -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/protocol" -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/protocol" -I"D:/Github/Course-Project-1/firmware/stm32/node_stm32_ver1/filter" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -fshort-enums -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-filter

clean-filter:
	-$(RM) ./filter/ema.cyclo ./filter/ema.d ./filter/ema.o ./filter/ema.su ./filter/kalman.cyclo ./filter/kalman.d ./filter/kalman.o ./filter/kalman.su

.PHONY: clean-filter

